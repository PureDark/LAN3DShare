#include "audio/AudioTelemetry.h"
#include <mutex>
#include "Injector.h"
#include "AsyncProcessScan.h"
#include "SourceProcess.h"
#include "Workers.h"
#include "WorkerEvents.h"
#include "capture/CaptureApi.h"
#include "capture/NativeGpuHooks.h"
#include "capture/DesktopCapture.h"
#include "common/FrameBus.h"
#include "common/GpuFrameBus.h"
#include "media/Codec.h"
#include "media/GpuAdapter.h"
#include "media/AdaptiveBitrate.h"
#include "audio/AudioCapture.h"
#include "net/Transport.h"
#include "ui/PreviewChannel.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>

namespace {
std::atomic<bool> quit{};
BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) { quit = true; return TRUE; }
    return FALSE;
}
bool ProgramCaptureReady(uint32_t pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module{sizeof(module)};
    bool runtime{};
    if (Module32FirstW(snapshot, &module)) do {
        if (!_wcsicmp(module.szModule, L"SimulatedRealityDirectX.dll")) { runtime = true; break; }
    } while (Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
    if (!runtime) return false;
    struct WindowState { DWORD pid; bool ready{}; } state{pid};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& state = *reinterpret_cast<WindowState*>(value);
        DWORD pid{}; GetWindowThreadProcessId(window, &pid);
        if (pid != state.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        DWORD_PTR result{};
        state.ready = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result) != 0;
        return state.ready ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.ready;
}
class FramePacer {
public:
    FramePacer() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }
    ~FramePacer() { if (timer_) CloseHandle(timer_); }
    void Wait(std::chrono::steady_clock::duration duration) {
        if (duration <= duration.zero()) return;
        LARGE_INTEGER due{}; due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100);
        if (timer_ && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, INFINITE);
        else std::this_thread::sleep_for(duration);
    }
private:
    HANDLE timer_{};
};
struct Options {
    uint32_t pid{}, stream{}, ordinal{}, fps{30}, bitrate{12000000}, width{1920}, height{1080};
    uint32_t frames{}, seconds{}, sourceWidth{2560}, sourceHeight{720}, captureFps{};
    uint16_t port{48730};
    bool pattern{}, list{}, noInject{}, stop{}, autoSource{}, recoverStream{}, noAudio{}, forceAudio{}, testAudio{}, profile{}, captureCpu{};
    int desktopMonitor{-1};
    std::string bind{"0.0.0.0"};
    std::string codec{"software"}, gpu{"auto"};
    std::wstring capture;
    std::wstring launch, arguments;
    std::wstring stopEvent, readyEvent;
    std::string sessionToken;
};
uint32_t CaptureFps(const Options& options, uint32_t outputFps, bool gpu) {
    if (options.captureFps) return options.captureFps;
    // A small producer margin absorbs frame-boundary and encoder jitter while
    // the sender still limits encoded output to the requested frame rate.
    return gpu ? std::min(240u, (outputFps * 6u + 4u) / 5u) : outputFps;
}
std::string Utf8(const std::wstring& text) {
    const auto n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n, nullptr, nullptr);
    return result;
}
uint32_t Number(const wchar_t* value, uint32_t maximum) {
    if (!value || !*value || *value == L'-') throw std::runtime_error("Expected a non-negative integer");
    size_t used{};
    const auto result = std::stoull(value, &used);
    if (used != wcslen(value) || result > maximum) throw std::runtime_error("Numeric argument outside supported range");
    return static_cast<uint32_t>(result);
}
void Help() {
    std::cout << "LAN3DShare Sender 0.7.9 (Windows x64)\n"
        "  --list                              List processes using LeiaSR DirectX\n"
        "  --pid PID                           Capture a selected application\n"
        "  --launch EXE [--args TEXT]           Start or reuse an application and follow its LeiaSR process\n"
        "  --test-pattern                      Send an independent stereo test image\n"
        "  --desktop-monitor N                 Share an ordinary desktop monitor (0-based)\n"
        "  --auto-source                       Prefer foreground LeiaSR SBS on that monitor; otherwise desktop\n"
        "  --listen IPv4 --port 48730           Bind address (default 0.0.0.0)\n"
        "  --stream N --weave N                 Select weaver / weave ordinal; 0 = auto\n"
        "  --width 1920 --height 1080           Maximum encoded frame dimensions (TOTAL for SBS)\n"
        "  --fps 60 --bitrate 100000000         Frame rate and bitrate targets\n"
        "  --capture-fps N                     Diagnostic limit 0..240; 0 = GPU +20% headroom / CPU output FPS\n"
        "  --bitrate 0                         Adapt NVENC to TCP backpressure (20..1000 Mbps)\n"
        "  --recover-stream                    Recover stalled video using a fresh keyframe connection\n"
        "  --no-audio                          Disable default system-output loopback audio\n"
        "  --audio                             Also capture system audio with the diagnostic test pattern\n"
        "  --codec auto|av1|hevc|h264|software  Auto negotiates NVENC/NVDEC; default diagnostic software\n"
        "  --frames N --seconds N              Stop after sent frame count / time\n"
        "  --profile                           Report sender stage timings once per second\n"
        "  --source-width 2560 --source-height 720   Test-pattern dimensions\n"
        "  --no-inject                         Read an already active capture bus\n"
        "  --capture-dll PATH                  Select matching capture DLL\n"
        "  --stop --pid PID                    Stop capture in that process\n"
        "Ctrl+C stops sending and capture. No game files are modified.\n";
}
lan3d::Frame Pattern(uint32_t width, uint32_t height, uint64_t sequence) {
    lan3d::Frame f{width, height, width * 4, sequence, lan3d::Timestamp100ns(), {}};
    f.bgra.resize(size_t(f.stride) * height);
    const uint32_t eyeWidth = width / 2;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const auto eye = x >= eyeWidth; const auto localX = x % eyeWidth;
        auto* p = f.bgra.data() + size_t(y) * f.stride + x * 4;
        p[0] = eye ? 200 : 30; p[1] = eye ? 150 : 35; p[2] = eye ? 25 : 200; p[3] = 255;
        const auto marker = uint32_t(sequence * 5) % eyeWidth;
        if ((localX > marker && localX < marker + 12) || y < 6 || y + 6 >= height) p[0] = p[1] = p[2] = 230;
        // Large L and R-like eye markers, plus identical frame-counter stripes.
        const auto gx = localX * 100 / eyeWidth, gy = y * 100 / height;
        const bool mark = gx >= 35 && gx <= 40 && gy >= 25 && gy <= 70;
        const bool foot = !eye && gy >= 65 && gy <= 70 && gx >= 35 && gx <= 65;
        const bool right = eye && ((gy >= 25 && gy <= 30 && gx >= 35 && gx <= 65) ||
            (gy >= 43 && gy <= 48 && gx >= 35 && gx <= 65) || (gx >= 60 && gx <= 65 && gy >= 25 && gy <= 48) ||
            (gy >= 48 && gy <= 70 && gx >= gy - 8 && gx <= gy - 2));
        const bool bit = gy > 85 && gy < 92 && ((sequence >> std::min(gx / 7, 13u)) & 1);
        if (mark || foot || right || bit) p[0] = p[1] = p[2] = 250;
    }
    return f;
}
lan3d::Frame Scale(lan3d::Frame source, uint32_t maxWidth, uint32_t maxHeight) {
    const double ratio = std::min({1.0, double(maxWidth) / source.width, double(maxHeight) / source.height});
    const auto width = std::max(4u, uint32_t(source.width * ratio) & ~3u);
    const auto height = std::max(2u, uint32_t(source.height * ratio) & ~1u);
    if (width == source.width && height == source.height) return source;
    lan3d::Frame dest{width, height, width * 4, source.sequence, source.timestamp100ns, {}};
    dest.kind = source.kind;
    dest.bgra.resize(size_t(dest.stride) * height);
    // Keep eyes separate; interpolation can never blend across the SBS seam.
    const uint32_t eyes = source.kind == lan3d::ContentKind::LeiaSbs ? 2 : 1;
    const uint32_t inEye = source.width / eyes, outEye = width / eyes;
    for (uint32_t eye = 0; eye < eyes; ++eye) for (uint32_t y = 0; y < height; ++y) {
        const double sy = std::max(0.0, (y + 0.5) * source.height / height - 0.5);
        const auto y0 = std::min(uint32_t(sy), source.height - 1), y1 = std::min(y0 + 1, source.height - 1);
        const double fy = sy - y0;
        for (uint32_t x = 0; x < outEye; ++x) {
            const double sx = std::max(0.0, (x + 0.5) * inEye / outEye - 0.5);
            const auto x0 = std::min(uint32_t(sx), inEye - 1), x1 = std::min(x0 + 1, inEye - 1);
            const double fx = sx - x0;
            auto* d = dest.bgra.data() + size_t(y) * dest.stride + size_t(eye * outEye + x) * 4;
            const auto* a = source.bgra.data() + size_t(y0) * source.stride + size_t(eye * inEye + x0) * 4;
            const auto* b = source.bgra.data() + size_t(y0) * source.stride + size_t(eye * inEye + x1) * 4;
            const auto* c = source.bgra.data() + size_t(y1) * source.stride + size_t(eye * inEye + x0) * 4;
            const auto* e = source.bgra.data() + size_t(y1) * source.stride + size_t(eye * inEye + x1) * 4;
            for (unsigned channel = 0; channel < 3; ++channel)
                d[channel] = uint8_t((a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
                    (c[channel] * (1 - fx) + e[channel] * fx) * fy + 0.5);
            d[3] = 255;
        }
    }
    return dest;
}
struct SenderProfile {
    using Clock = std::chrono::steady_clock;
    enum Stage { Read, Source, ScaleFrame, Configure, Preview, Encode, Send, Loop, GpuInput, GpuSubmit, GpuWait,
        GpuFlush, GpuMapInput, GpuEncodePicture, SourceCopyGpu, DestinationCopyGpu, StageCount };
    struct Sample { uint64_t count{}; double total{}, maximum{}; };
    explicit SenderProfile(bool enabled) : enabled(enabled), began(Clock::now()), window(began) {}
    bool enabled{};
    Clock::time_point began, window;
    std::array<Sample, StageCount> samples{}, totals{};
    uint64_t sent{}, totalSent{};
    uint32_t sourceWidth{}, sourceHeight{}, outputWidth{}, outputHeight{};
    void Add(Stage stage, double milliseconds) {
        for (auto* set : {&samples, &totals}) {
            auto& sample = (*set)[stage]; ++sample.count; sample.total += milliseconds;
            sample.maximum = std::max(sample.maximum, milliseconds);
        }
    }
    void Print(bool summary = false) {
        if (!enabled) return;
        const auto now = Clock::now();
        const auto seconds = std::chrono::duration<double>(now - (summary ? began : window)).count();
        if (!summary && seconds < 1.0) return;
        const auto& set = summary ? totals : samples;
        if (!set[Loop].count) return;
        static constexpr const char* names[] = {"read", "source", "scale", "configure", "preview", "encode", "send", "loop", "gpu_input_submit", "gpu_encode_submit", "gpu_bitstream_wait",
            "gpu_flush", "gpu_map_input", "gpu_encode_picture", "source_copy_gpu", "destination_copy_gpu"};
        std::cout << (summary ? "PROFILE_SUMMARY" : "PROFILE") << " seconds=" << seconds
            << " sent=" << (summary ? totalSent : sent) << " fps=" << (summary ? totalSent : sent) / seconds
            << " input=" << sourceWidth << 'x' << sourceHeight << " output=" << outputWidth << 'x' << outputHeight;
        for (size_t i = 0; i < set.size(); ++i) {
            const auto& s = set[i];
            std::cout << ' ' << names[i] << "_ms=" << (s.count ? s.total / s.count : 0.0)
                << '/' << s.maximum << '/' << s.count;
        }
        std::cout << " units=mean/max/count source_includes_read=1 loop_excludes_pacing=1\n" << std::flush;
        if (!summary) { samples = {}; sent = 0; window = now; }
    }
    struct Scope {
        SenderProfile& profile; Stage stage; Clock::time_point start;
        Scope(SenderProfile& profile, Stage stage) : profile(profile), stage(stage), start(profile.enabled ? Clock::now() : Clock::time_point{}) {}
        ~Scope() {
            if (profile.enabled) {
                profile.Add(stage, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                if (stage == Loop) profile.Print();
            }
        }
    };
};
}

int lan3d::RunSender(int argc, wchar_t** argv) try {
    quit = false;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Options o;
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, _countof(exe));
    o.capture = (std::filesystem::path(exe).parent_path() / L"LAN3DCapture.dll").wstring();
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const auto next = [&]() -> const wchar_t* { if (++i >= argc) throw std::runtime_error("Missing argument value"); return argv[i]; };
        if (a == L"--help" || a == L"-h") { Help(); return 0; }
        else if (a == L"--list-gpus") {
            for (const auto& gpu : lan3d::media::EncodeAdapters()) std::cout << gpu.index << " " << gpu.id << " " << Utf8(gpu.name) << '\n';
            return 0;
        }
        else if (a == L"--list") o.list = true;
        else if (a == L"--test-pattern") o.pattern = true;
        else if (a == L"--auto-source") o.autoSource = true;
        else if (a == L"--recover-stream") o.recoverStream = true;
        else if (a == L"--no-audio") o.noAudio = true;
        else if (a == L"--audio") o.forceAudio = true;
        else if (a == L"--test-audio") o.testAudio = true;
        else if (a == L"--gpu") o.gpu = Utf8(next());
        else if (a == L"--profile") o.profile = true;
        else if (a == L"--capture-cpu") o.captureCpu = true;
        else if (a == L"--desktop-monitor") o.desktopMonitor = int(Number(next(), 128));
        else if (a == L"--no-inject") o.noInject = true;
        else if (a == L"--stop") o.stop = true;
        else if (a == L"--pid") o.pid = Number(next(), UINT32_MAX);
        else if (a == L"--launch") o.launch = std::filesystem::absolute(next()).wstring();
        else if (a == L"--args") o.arguments = next();
        else if (a == L"--stream") o.stream = Number(next(), UINT32_MAX);
        else if (a == L"--weave") o.ordinal = Number(next(), 10000);
        else if (a == L"--fps") o.fps = Number(next(), 240);
        else if (a == L"--capture-fps") o.captureFps = Number(next(), 240);
        else if (a == L"--bitrate") o.bitrate = Number(next(), 1000000000);
        else if (a == L"--codec") o.codec = Utf8(next());
        else if (a == L"--width") o.width = Number(next(), 4096);
        else if (a == L"--height") o.height = Number(next(), 2160);
        else if (a == L"--source-width") o.sourceWidth = Number(next(), lan3d::MaxWidth);
        else if (a == L"--source-height") o.sourceHeight = Number(next(), lan3d::MaxHeight);
        else if (a == L"--port") o.port = static_cast<uint16_t>(Number(next(), 65535));
        else if (a == L"--listen") o.bind = Utf8(next());
        else if (a == L"--frames") o.frames = Number(next(), UINT32_MAX);
        else if (a == L"--seconds") o.seconds = Number(next(), 86400);
        else if (a == L"--stop-event") o.stopEvent = next();
        else if (a == L"--ready-event") o.readyEvent = next();
        else if (a == L"--session-token") o.sessionToken = Utf8(next());
        else if (a == L"--capture-dll") o.capture = std::filesystem::absolute(next()).wstring();
        else throw std::runtime_error("Unknown argument: " + Utf8(a));
    }
    if (o.list) {
        for (const auto& p : lan3d::ListProcesses()) if (p.leiaLoaded)
            std::cout << p.id << "  " << Utf8(p.name) << "  SimulatedRealityDirectX.dll loaded\n";
        std::cout << "Use --pid PID to attach to an existing stereo application. Keep its LeiaSR rendering active.\n";
        return 0;
    }
    if (o.autoSource && o.desktopMonitor < 0) o.desktopMonitor = 0;
    if (o.codec != "auto" && o.codec != "av1" && o.codec != "hevc" && o.codec != "h264" && o.codec != "software")
        throw std::runtime_error("Invalid codec; choose auto, av1, hevc, h264, or software");
    const bool autoPickProcess = o.autoSource && !o.pid && o.launch.empty();
    const bool desktopOnlySource = o.desktopMonitor >= 0 && !o.pid && o.launch.empty();
    if (!o.pid && !o.pattern && o.launch.empty() && !desktopOnlySource) { Help(); return argc == 1 ? 0 : 2; }
    if ((unsigned(o.pid != 0) + unsigned(o.pattern) + unsigned(!o.launch.empty()) + unsigned(desktopOnlySource) != 1) ||
        (o.noInject && !o.launch.empty()) || (o.stop && !o.pid) ||
        !o.fps || (o.bitrate && o.bitrate < 100000) || o.width < 48 || o.height < 48 ||
        o.sourceWidth < 4 || (o.sourceWidth & 1) || o.sourceHeight < 2 || (o.testAudio && !o.pattern) ||
        (o.noAudio && (o.forceAudio || o.testAudio)))
        throw std::runtime_error("Invalid source, size, bitrate, fps or port");
    if (!o.sessionToken.empty() && !lan3d::net::ValidSessionToken(o.sessionToken))
        throw std::runtime_error("Session token must contain exactly 32 hexadecimal characters");
    const bool recoverStream = o.recoverStream || !o.sessionToken.empty();
    const bool systemAudio = !o.noAudio && (!o.pattern || o.forceAudio) && !o.testAudio;
    const uint32_t sendDeadlineMs = recoverStream ? 250u : 1500u;
    lan3d::WorkerEvent stopEvent(o.stopEvent, SYNCHRONIZE);
    lan3d::WorkerEvent readyEvent(o.readyEvent, EVENT_MODIFY_STATE);
    const auto stopping = [&]() { return quit.load() || stopEvent.IsSet(); };
    if (stopping()) return 0;
    LUID selectedGpu{}; bool explicitGpu = o.gpu != "auto";
    if (explicitGpu) {
        const auto adapters = lan3d::media::EncodeAdapters();
        const auto found = std::find_if(adapters.begin(), adapters.end(), [&](const auto& gpu) { return gpu.id == o.gpu || std::to_string(gpu.index) == o.gpu; });
        if (found == adapters.end()) throw std::runtime_error("Selected NVIDIA GPU is unavailable; select an available GPU in settings");
        selectedGpu = found->luid;
        std::cout << "GPU_SELECTED index=" << found->index << " id=" << found->id << " name=" << Utf8(found->name) << '\n' << std::flush;
    }
    std::string error;
    uint32_t result{};
    struct LaunchedProcess {
        PROCESS_INFORMATION info{};
        bool resumed{};
        ~LaunchedProcess() {
            if (info.hProcess) {
                if (!resumed) TerminateProcess(info.hProcess, 1);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } launched;
    lan3d::PinnedProcess sourceProcess;
    const auto pinPid = [&](uint32_t pid) {
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!handle) { error = "Cannot open selected source process"; return false; }
        lan3d::ProcessIdentity identity;
        const bool read = lan3d::ReadProcessIdentity(handle, identity, error);
        CloseHandle(handle);
        if (!read) return false;
        auto pinned = lan3d::OpenValidated(identity, error);
        if (!pinned) return false;
        sourceProcess = std::move(pinned); return true;
    };
    if (!o.launch.empty()) {
        if (!std::filesystem::is_regular_file(o.launch)) throw std::runtime_error("Application executable does not exist");
        const auto matches = lan3d::FindMatchingProcesses(o.launch, error);
        if (!error.empty()) throw std::runtime_error(error);
        if (matches.size() > 1) throw std::runtime_error("More than one instance of this application is running; select the specific running 3D process");
        if (matches.size() == 1) {
            sourceProcess = lan3d::OpenValidated(matches.front(), error);
            if (!sourceProcess) throw std::runtime_error(error);
            o.pid = sourceProcess.identity().pid;
            std::cout << "SOURCE_PROCESS reused pid=" << o.pid << " same_executable=1\n" << std::flush;
        } else {
            if (!lan3d::LaunchForCapture(o.launch, o.arguments, launched.info, error)) throw std::runtime_error(error);
            o.pid = launched.info.dwProcessId;
            if (!pinPid(o.pid)) throw std::runtime_error(error);
        }
    }
    else if (o.pid && !pinPid(o.pid)) throw std::runtime_error(error);
    if (o.stop) {
        if (!lan3d::CallCapture(o.pid, o.capture, "LAN3DStop", nullptr, 0, result, error, &sourceProcess.identity()) || result)
            throw std::runtime_error(error.empty() ? "Capture stop failed: " + std::to_string(result) : error);
        std::cout << "Capture stopped; the DLL remains loaded until the source exits.\n"; return 0;
    }
    lan3d::FrameBus bus;
    lan3d::GpuFrameBus gpuBus;
    bool busActive{}, gpuBusActive{};
    bool requestGpu = !o.captureCpu && o.codec != "software";
    lan3d::DesktopCapture desktop;
    if (o.desktopMonitor >= 0 && !desktop.Open(unsigned(o.desktopMonitor), error)) throw std::runtime_error(error);
    bool started = false;
    struct Stopper {
        Options& options; bool& started;
        lan3d::PinnedProcess& sourceProcess;
        void Forget() {
            started = false;
        }
        bool Stop() {
            if (!started) return true;
            if (sourceProcess && !sourceProcess.Alive()) {
                Forget();
                std::cout << "Capture ended: source PID " << options.pid << " has exited\n" << std::flush;
                return true;
            }
            std::string error; uint32_t code{};
            const bool stopped = sourceProcess && lan3d::CallCapture(options.pid, options.capture, "LAN3DStop", nullptr, 0, code, error, &sourceProcess.identity()) && !code;
            if (stopped) {
                Forget();
                std::cout << "Capture stopped in PID " << options.pid << "; the source application remains running\n" << std::flush;
            } else {
                std::cerr << "Capture stop NOT CONFIRMED in PID " << options.pid << ": "
                    << (error.empty() ? "LAN3DStop result " + std::to_string(code) : error) << '\n' << std::flush;
            }
            return stopped;
        }
        ~Stopper() {
            try { Stop(); } catch (...) { std::cerr << "Capture stop NOT CONFIRMED: cleanup raised an exception\n"; }
        }
    } stopper{o, started, sourceProcess};
    std::optional<lan3d::NativeGpuHooks> nativeHooks;
    const auto attachCapture = [&]() {
        error.clear(); result = 0;
        if (!nativeHooks) {
            lan3d::NativeGpuHooks profile;
            if (!lan3d::ProbeNativeGpuHooks(profile, error)) return false;
            nativeHooks = profile;
            std::cout << "GPU_PROBE external " << error << '\n' << std::flush;
            error.clear();
        }
        if (!sourceProcess || !sourceProcess.Alive()) { error = "Selected source process has exited"; return false; }
        if (!lan3d::InjectCapture(o.pid, o.capture, error, &sourceProcess.identity())) return false;
        if (!lan3d::CallCapture(o.pid, o.capture, "LAN3DSetGpuHooks", &*nativeHooks, sizeof(*nativeHooks), result, error, &sourceProcess.identity()) || result) {
            if (error.empty()) error = "LAN3DSetGpuHooks result " + std::to_string(result);
            return false;
        }
        lan3d::CaptureConfig config;
        config.streamId = o.stream; config.weaveOrdinal = o.ordinal; config.maxFps = CaptureFps(o, o.fps, requestGpu);
        config.reserved = requestGpu ? lan3d::CaptureGpuOutput : 0;
        if (!lan3d::CallCapture(o.pid, o.capture, "LAN3DConfigure", &config, sizeof(config), result, error, &sourceProcess.identity()) || result) {
            if (error.empty()) error = "LAN3DConfigure result " + std::to_string(result);
            return false;
        }
        if (!lan3d::CallCapture(o.pid, o.capture, "LAN3DStart", nullptr, 0, result, error, &sourceProcess.identity()) || result) {
            if (error.empty()) error = "LAN3DStart result " + std::to_string(result);
            return false;
        }
        started = true;
        return true;
    };
    if (o.pid && o.launch.empty() && !o.noInject && !attachCapture()) {
        if (!o.autoSource || !o.launch.empty()) throw std::runtime_error(error.empty() ? "Capture initialization failed" : error);
        std::cout << "Automatic stereo capture unavailable: " << error << "; sharing ordinary screen\n" << std::flush;
    }
    if (o.pid && (started || o.noInject)) {
        bool opened = false;
        for (unsigned i = 0; i < 50 && !stopping(); ++i) {
            if (bus.Open(o.pid, false, error)) { opened = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!opened) { if (stopping()) return 0; throw std::runtime_error(error); }
        busActive = opened;
        gpuBusActive = requestGpu && gpuBus.Open(o.pid, false, error);
        if (requestGpu && !gpuBusActive) throw std::runtime_error(error);
    }
    // Program mode uses hot attachment only after the application has a
    // responsive window and LeiaSR. Do not install hooks in the launcher or
    // while the replacement game is still initializing graphics under loaders.
    if (launched.info.hThread) {
        if (ResumeThread(launched.info.hThread) == DWORD(-1)) throw std::runtime_error("Cannot resume launched application");
        launched.resumed = true;
        std::cout << "Application started; waiting for LeiaSR, PID " << o.pid << '\n' << std::flush;
    }
    lan3d::net::TcpServer server;
    std::cout << "SOURCE_SELECTED pid=" << o.pid << " monitor=" << o.desktopMonitor
        << " auto=" << (autoPickProcess ? 1 : 0) << " bus=" << (busActive ? 1 : 0);
    if (o.pid) for (const auto& process : lan3d::ListProcesses()) if (process.id == o.pid) {
        std::cout << " name=" << Utf8(process.name); break;
    }
    std::cout << '\n' << std::flush;
    if (!server.Listen(o.bind, o.port, error)) throw std::runtime_error(error);
    std::cout << "Listening on " << o.bind << ':' << server.port() << "; one receiver; codec " << o.codec << ", "
        << o.fps << " FPS target, max frame " << o.width << 'x' << o.height << "\n" << std::flush;
    readyEvent.Signal();
    const auto start = std::chrono::steady_clock::now();
    const auto expired = [&]() { return o.seconds && std::chrono::steady_clock::now() - start >= std::chrono::seconds(o.seconds); };
    uint64_t sent{}, lastSequence{}, patternSequence{}, outputSequence{}, lastGpuTimestamp{}, sourceGeneration{};
    auto nextDiscovery = start;
    std::unique_ptr<lan3d::AsyncProcessScan> processScan;
    if (autoPickProcess) processScan = std::make_unique<lan3d::AsyncProcessScan>();
    auto programReadySince = std::chrono::steady_clock::time_point{};
    lan3d::Frame cachedSbs, cachedDesktop;
    std::string programState;
    // Steam may exit the initially launched process and create the real game
    // through its service. Follow only the selected executable's full path.
    const auto refreshProgramSource = [&]() {
        if (o.launch.empty() || std::chrono::steady_clock::now() < nextDiscovery) return;
        nextDiscovery = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (sourceProcess && sourceProcess.Alive() && started && busActive) return;
        if (!sourceProcess || !sourceProcess.Alive()) {
            if (!stopper.Stop()) return;
            if (sourceProcess || o.pid) ++sourceGeneration;
            bus.Close(); gpuBus.Close();
            busActive = gpuBusActive = false; o.pid = 0;
            cachedSbs = {}; cachedDesktop = {}; lastSequence = lastGpuTimestamp = 0;
            sourceProcess = lan3d::PinnedProcess{};
            programReadySince = {};
            programState = "waiting-process";
            const auto matches = lan3d::FindMatchingProcesses(o.launch, error);
            if (matches.size() != 1 || !error.empty()) {
                std::cout << "SOURCE_STATE waiting-process\nSOURCE_PROCESS waiting reason="
                    << (!error.empty() ? "query-failed" : matches.empty() ? "not-running" : "ambiguous")
                    << " matches=" << matches.size() << '\n' << std::flush;
                return;
            }
            sourceProcess = lan3d::OpenValidated(matches.front(), error);
            if (!sourceProcess) return;
            o.pid = sourceProcess.identity().pid;
            ++sourceGeneration;
            std::cout << "SOURCE_PROCESS followed pid=" << o.pid << " same_executable=1\n" << std::flush;
        }
        if (!started) {
            const auto now = std::chrono::steady_clock::now();
            if (!ProgramCaptureReady(o.pid)) programReadySince = {};
            else if (programReadySince == std::chrono::steady_clock::time_point{}) programReadySince = now;
            if (programReadySince == std::chrono::steady_clock::time_point{} || now - programReadySince < std::chrono::seconds(2)) {
                if (programState != "waiting-runtime") std::cout << "SOURCE_STATE waiting-runtime pid=" << o.pid << '\n' << std::flush;
                programState = "waiting-runtime";
                return;
            }
        }
        const bool attached = started || attachCapture();
        busActive = attached && bus.Open(o.pid, false, error);
        gpuBusActive = requestGpu && busActive && gpuBus.Open(o.pid, false, error);
        if (requestGpu && !gpuBusActive) busActive = false;
        programState = busActive ? "" : "capture-unavailable";
        if (!busActive) std::cout << "SOURCE_STATE capture-unavailable\nSOURCE_ATTACH_FAILED pid=" << o.pid << " error=" << error << '\n' << std::flush;
        else std::cout << "SOURCE_SELECTED pid=" << o.pid << " monitor=" << o.desktopMonitor
            << " auto=0 bus=1 followed=1\nSOURCE_STATE waiting-input\n" << std::flush;
    };
    std::vector<HMONITOR> monitors;
    std::wstring selectedMonitorName;
    HMONITOR selectedMonitor{};
    const auto refreshMonitors = [&]() {
        monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM value) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(value)->push_back(m); return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
        if (selectedMonitorName.empty() && size_t(o.desktopMonitor) < monitors.size()) {
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitors[o.desktopMonitor], &info)) selectedMonitorName = info.szDevice;
        }
        selectedMonitor = nullptr;
        for (const auto monitor : monitors) {
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) && selectedMonitorName == info.szDevice) { selectedMonitor = monitor; break; }
        }
    };
    refreshMonitors();
    std::string previousStatus;
    auto lastStatus = start - std::chrono::seconds(2);
    std::string previousDesktopError;
    auto lastDesktopError = start - std::chrono::seconds(5);
    lan3d::ui::PreviewPublisher preview;
    const auto secondsNow = []() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    // This state belongs to the sender session, not one media connection. A
    // reconnect must not undo the reduction that triggered its recovery.
    lan3d::media::AdaptiveBitrate automaticBitrate;
    automaticBitrate.Reset(o.fps, lan3d::media::AdaptiveBitrate::Initial, secondsNow());
    while (!stopping() && !expired() && (!o.frames || sent < o.frames)) {
        refreshProgramSource();
        lan3d::net::TcpConnection connection;
        const auto accepted = server.Accept(connection, 250, error);
        if (accepted == lan3d::net::IoResult::Timeout) {
            if (busActive) {
                const auto status = bus.Status();
                if (!status.empty() && status != previousStatus) { previousStatus = status; std::cout << "Capture: " << status << '\n' << std::flush; }
            }
            continue;
        }
        if (accepted != lan3d::net::IoResult::Ok) throw std::runtime_error(error);
        if (!o.sessionToken.empty() && connection.AuthenticateServer(o.sessionToken, 1500, error) != lan3d::net::IoResult::Ok) {
            std::cout << "Receiver session rejected: " << error << '\n' << std::flush;
            continue;
        }
        if (stopping()) break;
        std::cout << "Receiver connected\n" << std::flush;
        uint32_t peerHardware{};
        if (connection.ReceiveCapabilities(peerHardware, 6000, error) != lan3d::net::IoResult::Ok) {
            std::cout << "Receiver capabilities rejected: " << error << '\n' << std::flush;
            continue;
        }
        using lan3d::media::CodecId;
        const uint32_t localHardware = o.codec == "software" ? 0 : lan3d::media::HardwareEncodeCapabilities();
        uint32_t commonHardware = localHardware & peerHardware;
        if (o.codec == "av1") commonHardware &= lan3d::media::CodecBit(CodecId::Av1);
        else if (o.codec == "hevc") commonHardware &= lan3d::media::CodecBit(CodecId::Hevc);
        else if (o.codec == "h264") commonHardware &= lan3d::media::CodecBit(CodecId::H264);
        if (!commonHardware && o.codec != "software" && !o.captureCpu)
            throw std::runtime_error("Requested codec is not supported by both NVIDIA encoder and receiver hardware decoder");
        uint32_t streamWidth = o.width, streamHeight = o.height, streamFps = o.fps;
        uint32_t streamBitrate = o.bitrate ? o.bitrate : automaticBitrate.target();
        if (!commonHardware && o.codec != "software") {
            streamWidth = std::min(streamWidth, 1920u); streamHeight = std::min(streamHeight, 1080u);
            streamFps = std::min(streamFps, 30u); streamBitrate = std::min(streamBitrate, 20000000u);
            std::cout << "FALLBACK Software H.264: no mutually supported hardware codec; target limited to 1080p30/20 Mbps\n" << std::flush;
        }
        if (!commonHardware && !o.bitrate) streamBitrate = lan3d::media::AdaptiveBitrate::Minimum;
        const bool connectionGpu = !o.captureCpu && commonHardware;
        if (requestGpu != connectionGpu) {
            requestGpu = connectionGpu;
            if (started) {
                lan3d::CaptureConfig c; c.streamId = o.stream; c.weaveOrdinal = o.ordinal; c.maxFps = CaptureFps(o, streamFps, requestGpu);
                c.reserved = requestGpu ? lan3d::CaptureGpuOutput : 0;
                if (!lan3d::CallCapture(o.pid, o.capture, "LAN3DConfigure", &c, sizeof(c), result, error, &sourceProcess.identity()) || result) {
                    if (!o.launch.empty() && !sourceProcess.Alive()) {
                        nextDiscovery = {}; refreshProgramSource();
                    } else throw std::runtime_error("Cannot switch capture memory path: " + error);
                }
            }
            gpuBusActive = requestGpu && busActive && gpuBus.Open(o.pid, false, error);
        }
        bool adaptBitrate = !o.bitrate;
        lastSequence = 0;
        if (o.desktopMonitor >= 0) { cachedDesktop = {}; desktop.RequestRefresh(); }
        lan3d::Frame latestFrame;
        bool stereoSelected{};
        std::string previousSourceState;
        auto sourceStateAt = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        uint64_t desktopFreshAfter{};
        auto lastFrameSent = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        lan3d::media::Encoder encoder;
        // A new connection starts a new capture queue. No sound retained from
        // a disconnected session may accompany its new video keyframe.
        lan3d::audio::LoopbackCapture audioCapture;
        if (systemAudio) {
            try { audioCapture.Start(); }
            catch (const std::exception& e) { std::cerr << "AUDIO_UNAVAILABLE " << e.what() << '\n' << std::flush; }
        }
        std::mutex audioMeterMutex;
        lan3d::audio::LevelMeter sentMeter;
        uint64_t sentAudioFrames{};
        lan3d::audio::TelemetryPublisher audioTelemetry([&] {
            const auto capture = audioCapture.Stats();
            lan3d::audio::Telemetry data; data.enabled = systemAudio || o.testAudio;
            data.ready = capture.ready || o.testAudio; data.input = capture.level;
            data.inputFrames = capture.capturedFrames; data.droppedFrames = capture.discardedFrames;
            data.timestampErrors = capture.timestampErrors; data.clockCorrected = capture.clockCorrected;
            const auto detail = audioCapture.Status(); strncpy_s(data.detail, detail.c_str(), _TRUNCATE);
            { std::lock_guard lock(audioMeterMutex); data.output = sentMeter.Read(GetTickCount64()); data.outputFrames = sentAudioFrames; }
            return data;
        });
        std::string previousAudioStatus;
        uint64_t audioFramesSent{}, audioPacketsSent{}, testAudioEnd{};
        bool streamReady{};
        uint32_t encodedWidth{}, encodedHeight{};
        bool encodedGpu{};
        uint32_t encodedSourceWidth{}, encodedSourceHeight{};
        LUID encodedAdapter{};
        lastGpuTimestamp = 0;
        auto connectionSourceGeneration = sourceGeneration;
        uint32_t displayWidth{}, displayHeight{};
        const auto releaseGpu = [&](uint64_t sequence) {
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                if (gpuBus.Release(sequence)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("Cannot release completed GPU frame lease");
        };
        auto nextFrame = std::chrono::steady_clock::now();
        auto period = std::chrono::nanoseconds(1000000000 / streamFps);
        SenderProfile profile(o.profile);
        FramePacer pacer;
        while (!stopping() && !expired() && connection.IsOpen() && (!o.frames || sent < o.frames)) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextFrame) { pacer.Wait(std::min(nextFrame - now, std::chrono::steady_clock::duration(std::chrono::milliseconds(5)))); continue; }
            nextFrame += period;
            if (nextFrame < now - period) nextFrame = now + period;
            SenderProfile::Scope loopTiming(profile, SenderProfile::Loop);
            lan3d::Frame source;
            lan3d::GpuFrame gpuFrame;
            bool gpuReady{};
            uint64_t gpuLease{};
            {
            SenderProfile::Scope sourceTiming(profile, SenderProfile::Source);
            const auto readLatest = [&](lan3d::Frame& frame) {
                SenderProfile::Scope readTiming(profile, SenderProfile::Read);
                return busActive && bus.ReadLatest(frame, lastSequence);
            };
            refreshProgramSource();
            if (connectionSourceGeneration != sourceGeneration) {
                latestFrame = {}; encodedWidth = encodedHeight = 0; stereoSelected = false;
                connectionSourceGeneration = sourceGeneration;
            }
            if (autoPickProcess && now >= nextDiscovery) {
                nextDiscovery = now + std::chrono::seconds(2);
                refreshMonitors();
                const auto foreground = GetForegroundWindow(); DWORD foregroundPid{};
                GetWindowThreadProcessId(foreground, &foregroundPid);
                const bool onMonitor = selectedMonitor && MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == selectedMonitor;
                if (onMonitor && foregroundPid && (foregroundPid != o.pid || (!started && !o.noInject) || !busActive)) {
                    if (!processScan->Request(foregroundPid)) nextDiscovery = now;
                }
            }
            if (processScan) if (auto scanResult = processScan->TryTake()) {
                const auto foreground = GetForegroundWindow(); DWORD foregroundPid{};
                GetWindowThreadProcessId(foreground, &foregroundPid);
                const auto& p = scanResult->info;
                const bool scanAccepted = scanResult->pid == foregroundPid && selectedMonitor &&
                    MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == selectedMonitor &&
                    p.leiaLoaded && _wcsicmp(p.name.c_str(), L"LAN3DShare.exe") && scanResult->process.Alive();
                std::cout << "SOURCE_SCAN pid=" << scanResult->pid << " scan_ms=" << scanResult->milliseconds
                    << " background=1 leia=" << p.leiaLoaded << " accepted=" << scanAccepted << '\n';
                if (scanAccepted) do {
                    if (p.id != o.pid || !sourceProcess ||
                        sourceProcess.identity().creationTime != scanResult->process.identity().creationTime) {
                        // Keep ownership of the previous capture until its
                        // stop is confirmed, so shutdown can retry it.
                        if (!stopper.Stop()) break;
                        if (!scanResult->process.Alive()) break;
                        sourceProcess = std::move(scanResult->process);
                        o.pid = p.id; cachedSbs = {}; lastSequence = 0; lastGpuTimestamp = 0; busActive = gpuBusActive = false;
                    }
                    const bool attached = started || o.noInject || attachCapture();
                    busActive = attached && bus.Open(o.pid, false, error);
                    gpuBusActive = requestGpu && busActive && gpuBus.Open(o.pid, false, error);
                    if (requestGpu && !gpuBusActive) busActive = false;
                    if (!busActive)
                        std::cout << "Automatic stereo capture unavailable: " << error << '\n' << std::flush;
                    else std::cout << "Stereo source selected: " << Utf8(p.name) << "; observing existing and new weavers\n" << std::flush;
                } while (false);
            }
            if (gpuBusActive && commonHardware) {
                const auto foreground = GetForegroundWindow(); DWORD foregroundPid{};
                GetWindowThreadProcessId(foreground, &foregroundPid);
                const bool foregroundMatches = !autoPickProcess || (foregroundPid == o.pid && selectedMonitor &&
                    MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == selectedMonitor);
                {
                    SenderProfile::Scope readTiming(profile, SenderProfile::Read);
                    gpuReady = gpuBus.AcquireLatest(gpuFrame, lastSequence);
                }
                const auto time = lan3d::Timestamp100ns();
                if (gpuReady) {
                    gpuLease = gpuFrame.sequence; lastSequence = gpuFrame.sequence;
                    if (!foregroundMatches || time < gpuFrame.timestamp100ns || time - gpuFrame.timestamp100ns > 2500000) {
                        releaseGpu(gpuLease); gpuLease = 0; gpuReady = false;
                    } else {
                        lastGpuTimestamp = gpuFrame.timestamp100ns;
                        if (!stereoSelected) std::cout << "SOURCE_KIND LeiaSbs\nSOURCE_STATE leia-sbs gpu_shared=1\n" << std::flush;
                        stereoSelected = true; cachedDesktop = {}; desktopFreshAfter = time;
                        profile.sourceWidth = gpuFrame.width; profile.sourceHeight = gpuFrame.height;
                        latestFrame = {};
                        // Requested dimensions describe the complete packed SBS.
                        latestFrame.width = std::min(gpuFrame.width, streamWidth) & ~3u;
                        latestFrame.height = std::min(gpuFrame.height, streamHeight) & ~1u;
                        latestFrame.kind = gpuFrame.kind;
                        latestFrame.sequence = gpuFrame.sequence;
                        latestFrame.timestamp100ns = gpuFrame.timestamp100ns;
                        displayWidth = gpuFrame.width / 2; displayHeight = gpuFrame.height;
                        if (o.desktopMonitor >= 0 || !o.launch.empty()) gpuFrame.sequence = ++outputSequence;
                    }
                }
                if (!gpuReady && (o.desktopMonitor < 0 || (foregroundMatches && lastGpuTimestamp &&
                    time >= lastGpuTimestamp && time - lastGpuTimestamp < 7500000))) {
                    if (now - lastStatus > std::chrono::seconds(2)) {
                        std::cout << "Capture: " << bus.Status() << '\n' << std::flush; lastStatus = now;
                    }
                    if (connection.PollPeerClosed()) {
                        if (recoverStream) std::cout << "STREAM_RECOVERY action=listen reason=idle-peer-closed\n" << std::flush;
                        break;
                    }
                    // A missing producer frame must not consume another whole
                    // 60 Hz output interval. Retry briefly until it is ready.
                    nextFrame -= period;
                    pacer.Wait(std::chrono::milliseconds(1));
                    continue;
                }
            }
            if (gpuReady) { /* GPU input is consumed without a CPU image below. */ }
            else if (o.pattern) source = Pattern(o.sourceWidth, o.sourceHeight, ++patternSequence);
            else if (o.desktopMonitor >= 0) {
                lan3d::Frame captured;
                const bool freshSbs = busActive && !gpuBusActive && readLatest(captured);
                if (freshSbs) { lastSequence = captured.sequence; cachedSbs = std::move(captured); }
                const auto foreground = GetForegroundWindow(); DWORD foregroundPid{}; GetWindowThreadProcessId(foreground, &foregroundPid);
                const bool onMonitor = selectedMonitor && MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == selectedMonitor;
                const bool foregroundMatches = !autoPickProcess || (foregroundPid == o.pid && onMonitor);
                const auto time = lan3d::Timestamp100ns();
                const bool stereo = busActive && !gpuBusActive && foregroundMatches && !cachedSbs.bgra.empty() && cachedSbs.kind == lan3d::ContentKind::LeiaSbs &&
                    time >= cachedSbs.timestamp100ns && time - cachedSbs.timestamp100ns < 7500000;
                const std::string sourceState = !programState.empty() ? programState : stereo ? "leia-sbs" : !o.pid ? "desktop" :
                    !busActive ? "capture-unavailable" : cachedSbs.bgra.empty() ? "waiting-input" :
                    !foregroundMatches ? "foreground-desktop" : cachedSbs.kind != lan3d::ContentKind::LeiaSbs ? "not-sbs" : "stale-sbs";
                if (sourceState != previousSourceState || now - sourceStateAt >= std::chrono::seconds(5)) {
                    previousSourceState = sourceState; sourceStateAt = now;
                    std::cout << "SOURCE_STATE " << sourceState << " pid=" << o.pid << " foreground=" << foregroundPid
                        << " monitor=" << o.desktopMonitor << " on_monitor=" << (onMonitor ? 1 : 0)
                        << " bus=" << (busActive ? 1 : 0) << " sbs_sequence=" << lastSequence
                        << " sbs_age_ms=" << (cachedSbs.bgra.empty() || time < cachedSbs.timestamp100ns ? -1LL :
                            static_cast<long long>((time - cachedSbs.timestamp100ns) / 10000)) << '\n' << std::flush;
                }
                if (stereo) {
                    if (!stereoSelected) {
                        cachedDesktop = {}; desktopFreshAfter = time;
                    }
                    if (freshSbs || latestFrame.bgra.empty() || latestFrame.kind != lan3d::ContentKind::LeiaSbs) source = cachedSbs;
                } else {
                    if (stereoSelected) desktop.RequestRefresh();
                    const bool desktopGpu = commonHardware && !o.captureCpu;
                    const auto desktopResult = desktopGpu ? desktop.CaptureGpu(gpuFrame, error) : desktop.Capture(captured, error);
                    if (desktopGpu && desktopResult == lan3d::DesktopCaptureResult::FrameReady && gpuFrame.timestamp100ns >= desktopFreshAfter) {
                        gpuReady = true; gpuFrame.sequence = ++outputSequence;
                        if (latestFrame.kind != lan3d::ContentKind::Desktop || !encodedGpu)
                            std::cout << "SOURCE_KIND Desktop\nSOURCE_STATE desktop gpu_shared=1\n" << std::flush;
                        profile.sourceWidth = gpuFrame.width; profile.sourceHeight = gpuFrame.height;
                        const double scale = std::min({1.0, double(streamWidth) / gpuFrame.width, double(streamHeight) / gpuFrame.height});
                        latestFrame = {}; latestFrame.width = uint32_t(gpuFrame.width * scale) & ~3u;
                        latestFrame.height = uint32_t(gpuFrame.height * scale) & ~1u;
                        latestFrame.sequence = gpuFrame.sequence; latestFrame.timestamp100ns = gpuFrame.timestamp100ns;
                        latestFrame.kind = lan3d::ContentKind::Desktop;
                        displayWidth = gpuFrame.width; displayHeight = gpuFrame.height;
                    } else if (desktopGpu && desktopResult != lan3d::DesktopCaptureResult::Error) {
                        if (connection.PollPeerClosed()) break;
                        nextFrame -= period; pacer.Wait(std::chrono::milliseconds(1)); continue;
                    }
                    if (desktopResult == lan3d::DesktopCaptureResult::Error) throw std::runtime_error(error);
                    if (!error.empty() && (error != previousDesktopError || now - lastDesktopError >= std::chrono::seconds(5))) {
                        std::cout << "Desktop capture temporarily unavailable: " << error << "; retrying\n" << std::flush;
                        previousDesktopError = error; lastDesktopError = now;
                    }
                    if (!desktopGpu && desktopResult == lan3d::DesktopCaptureResult::FrameReady && captured.timestamp100ns >= desktopFreshAfter) {
                        cachedDesktop = std::move(captured); source = cachedDesktop;
                    }
                    else if (systemAudio && error.empty() && desktop.CanRepeatCurrent() && !cachedDesktop.bgra.empty()) {
                        // DXGI confirms the desktop did not change. Refresh its
                        // timeline so music continues over a static desktop.
                        // A paused SBS producer never receives this treatment.
                        source = cachedDesktop; source.timestamp100ns = time;
                    }
                    else if (!cachedDesktop.bgra.empty() && (latestFrame.bgra.empty() || latestFrame.kind != lan3d::ContentKind::Desktop)) source = cachedDesktop;
                }
                stereoSelected = stereo;
                if (gpuReady) { /* GPU metadata already populated. */ }
                else if (!source.bgra.empty()) source.sequence = ++outputSequence;
                // Do not keep sending the previous content kind while waiting
                // for the first current desktop/SBS frame after a transition.
                else if (!latestFrame.bgra.empty() && latestFrame.kind != (stereo ? lan3d::ContentKind::LeiaSbs : lan3d::ContentKind::Desktop)) continue;
                else if (latestFrame.bgra.empty() || now - lastFrameSent < std::chrono::seconds(1)) continue;
            } else if (!readLatest(source)) {
                if (now - lastStatus > std::chrono::seconds(2)) {
                    const auto status = bus.Status();
                    if (!status.empty()) std::cout << "Capture: " << status << '\n' << std::flush;
                    lastStatus = now;
                }
                // A paused source still refreshes the receiver and detects a
                // disconnected peer. Keep both original sequence and timestamp.
                if (latestFrame.bgra.empty() || now - lastFrameSent < std::chrono::seconds(1)) continue;
            }
            }
            if (!source.bgra.empty()) {
                if (o.desktopMonitor < 0) {
                    lastSequence = source.sequence;
                    if (!o.launch.empty()) source.sequence = ++outputSequence;
                }
                if (latestFrame.bgra.empty() || latestFrame.kind != source.kind)
                    std::cout << "SOURCE_KIND " << (source.kind == lan3d::ContentKind::LeiaSbs ? "LeiaSbs" : "Desktop") << '\n' << std::flush;
                profile.sourceWidth = source.width; profile.sourceHeight = source.height;
                displayWidth = source.kind == lan3d::ContentKind::LeiaSbs ? source.width / 2 : source.width;
                displayHeight = source.height;
                SenderProfile::Scope scaleTiming(profile, SenderProfile::ScaleFrame);
                latestFrame = Scale(std::move(source), streamWidth, streamHeight);
            }
            const auto& frame = latestFrame;
            if (frame.width != encodedWidth || frame.height != encodedHeight || encodedGpu != gpuReady ||
                (gpuReady && (gpuFrame.width != encodedSourceWidth || gpuFrame.height != encodedSourceHeight ||
                    gpuFrame.adapterLuid.LowPart != encodedAdapter.LowPart || gpuFrame.adapterLuid.HighPart != encodedAdapter.HighPart))) {
                SenderProfile::Scope configureTiming(profile, SenderProfile::Configure);
                bool configured{};
                std::string failures;
                for (const auto codec : {CodecId::Av1, CodecId::Hevc, CodecId::H264}) {
                    if (!(commonHardware & lan3d::media::CodecBit(codec))) continue;
                    lan3d::media::CodecConfig config{frame.width, frame.height, streamFps, streamBitrate, {}, codec};
                    if (gpuReady ? encoder.ConfigureGpu(config, gpuFrame, error, explicitGpu ? &selectedGpu : nullptr) : encoder.Configure(config, true, error)) { configured = true; break; }
                    failures += std::string(lan3d::media::CodecName(codec)) + ": " + error + "; ";
                }
                if (!configured && gpuReady) { if (gpuLease) releaseGpu(gpuLease); gpuLease = 0; throw std::runtime_error(failures); }
                if (!configured && commonHardware && o.captureCpu && (o.codec == "auto" || o.codec == "h264")) {
                    std::cout << "FALLBACK Software H.264: hardware initialization failed: " << failures << '\n' << std::flush;
                    commonHardware = 0;
                    if (gpuBusActive) {
                        lan3d::CaptureConfig c; c.streamId = o.stream; c.weaveOrdinal = o.ordinal; c.maxFps = CaptureFps(o, std::min(streamFps, 30u), false);
                        if (started && (!lan3d::CallCapture(o.pid, o.capture, "LAN3DConfigure", &c, sizeof(c), result, error, &sourceProcess.identity()) || result))
                            throw std::runtime_error("Cannot enable CPU capture fallback: " + error);
                        requestGpu = gpuBusActive = false; lastSequence = lastGpuTimestamp = 0; cachedSbs = {};
                    }
                    streamWidth = std::min(streamWidth, 1920u); streamHeight = std::min(streamHeight, 1080u);
                    streamFps = std::min(streamFps, 30u); streamBitrate = std::min(streamBitrate, 20000000u);
                    latestFrame = Scale(std::move(latestFrame), streamWidth, streamHeight);
                    period = std::chrono::nanoseconds(1000000000 / streamFps); nextFrame = now + period;
                }
                if (!commonHardware) configured = encoder.Configure(frame.width, frame.height, streamFps, streamBitrate, error);
                if (!configured) throw std::runtime_error(failures.empty() ? error : failures);
                encodedWidth = frame.width; encodedHeight = frame.height; encodedGpu = gpuReady;
                if (gpuReady) { encodedSourceWidth = gpuFrame.width; encodedSourceHeight = gpuFrame.height; encodedAdapter = gpuFrame.adapterLuid; }
                if (!encoder.RequestKeyFrame(error)) throw std::runtime_error(error);
                std::cout << "ENCODER " << (encoder.hardware() ? "NVENC " : "Software ") << lan3d::media::CodecName(encoder.config().codec) << ' '
                    << (frame.kind == lan3d::ContentKind::LeiaSbs ? "SBS " : "Desktop ") << frame.width << 'x' << frame.height
                    << " target=" << streamFps << "fps/" << streamBitrate / 1000000 << "Mbps";
                if (gpuReady) std::cout << " gpu_shared=1";
                if (frame.kind == lan3d::ContentKind::LeiaSbs) std::cout << " (each eye " << frame.width / 2 << 'x' << frame.height << ')';
                std::cout << '\n' << std::flush;
                if (!o.bitrate) {
                    automaticBitrate.StartStream(streamFps, streamBitrate, secondsNow());
                    std::cout << "AUTO_BITRATE target=" << streamBitrate
                        << " reason=" << (encoder.hardware() ? "initial" : "software-fallback") << '\n' << std::flush;
                }
            }
            profile.outputWidth = frame.width; profile.outputHeight = frame.height;
            {
                SenderProfile::Scope previewTiming(profile, SenderProfile::Preview);
                if (!gpuReady) preview.Publish(frame);
            }
            std::vector<lan3d::media::EncodedFrame> packets;
            {
                SenderProfile::Scope encodeTiming(profile, SenderProfile::Encode);
                if (gpuReady) {
                    if (!encoder.EncodeGpu(gpuFrame, packets, error))
                        throw std::runtime_error("Shared GPU encoding failed (input quarantined): " + error);
                    if (o.profile) {
                        const auto timing = encoder.lastTiming();
                        profile.Add(SenderProfile::GpuInput, timing.inputSubmitMs);
                        profile.Add(SenderProfile::GpuSubmit, timing.encodeSubmitMs);
                        profile.Add(SenderProfile::GpuWait, timing.bitstreamWaitMs);
                        profile.Add(SenderProfile::GpuFlush, timing.flushMs);
                        profile.Add(SenderProfile::GpuMapInput, timing.mapInputMs);
                        profile.Add(SenderProfile::GpuEncodePicture, timing.encodePictureMs);
                        profile.Add(SenderProfile::SourceCopyGpu, timing.sourceCopyGpuMs);
                        profile.Add(SenderProfile::DestinationCopyGpu, timing.destinationCopyGpuMs);
                    }
                    if (gpuLease) releaseGpu(gpuLease); gpuLease = 0;
                } else if (!encoder.Encode(frame, packets, error)) throw std::runtime_error(error);
            }
            if (gpuReady) {
                lan3d::Frame thumbnail;
                if (encoder.GpuPreview(thumbnail)) preview.Publish(thumbnail);
            }
            lastFrameSent = now;
            lan3d::media::AdaptiveBitrate::Decision bitrateDecision{streamBitrate};
            for (auto& packet : packets) {
                packet.displayWidth = displayWidth; packet.displayHeight = displayHeight;
                if (systemAudio) packet.audio = audioCapture.TakeThrough(packet.timestamp100ns);
                else if (o.testAudio) {
                    // Silent, timestamped PCM exercises the complete production
                    // media path without playing tones or capturing user audio.
                    const auto span = uint64_t(10000000) / streamFps;
                    const auto first = testAudioEnd ? testAudioEnd : packet.timestamp100ns - span;
                    if (packet.timestamp100ns > first) {
                        const auto count = static_cast<uint32_t>(std::min<uint64_t>(lan3d::audio::MaxBlockFrames,
                            (packet.timestamp100ns - first) * lan3d::audio::SampleRate / 10000000));
                        packet.audio.timestamp100ns = packet.timestamp100ns - uint64_t(count) * 10000000 / lan3d::audio::SampleRate;
                        packet.audio.samples.resize(size_t(count) * lan3d::audio::Channels);
                        packet.audio.discontinuity = !testAudioEnd;
                        testAudioEnd = packet.audio.End100ns();
                        if (!count) packet.audio = {};
                    }
                }
                const auto sendStart = std::chrono::steady_clock::now();
                lan3d::net::IoResult status;
                {
                    SenderProfile::Scope sendTiming(profile, SenderProfile::Send);
                    status = connection.Send(packet, sendDeadlineMs, error);
                }
                if (status != lan3d::net::IoResult::Ok) {
                    // Transport closes on a partial packet; never continue that
                    // packet or reuse predicted frames from this encoder session.
                    const bool timedOut = status == lan3d::net::IoResult::Timeout ||
                        error.find("TCP send deadline exceeded") != std::string::npos;
                    connection.Close();
                    if (timedOut) {
                        if (!o.bitrate) {
                            automaticBitrate.StartStream(streamFps, streamBitrate, secondsNow());
                            const auto reduced = automaticBitrate.OnSendTimeout(secondsNow());
                            streamBitrate = reduced.target;
                            std::cout << "AUTO_BITRATE target=" << streamBitrate << " reason=send-timeout\n" << std::flush;
                        }
                        std::cout << "STREAM_BACKPRESSURE send_deadline_ms=" << sendDeadlineMs
                            << " recover=" << (recoverStream ? 1 : 0) << " action=reconnect target=" << streamBitrate
                            << " mode=" << (o.bitrate ? "manual" : "auto")
                            << " advice=" << (o.bitrate ? "lower-bitrate-or-use-auto" : "reduced-target")
                            << " detail=" << error << '\n' << std::flush;
                    } else {
                        // Disconnection is not bandwidth evidence. Keep the
                        // configured target even if the next socket recovers it.
                        if (!o.bitrate) automaticBitrate.StartStream(streamFps, streamBitrate, secondsNow());
                        std::cout << "Receiver disconnected: " << error << '\n' << std::flush;
                    }
                    if (recoverStream)
                        std::cout << "STREAM_RECOVERY action=listen reason=" << (timedOut ? "send-timeout" : "send-failure")
                            << " target=" << streamBitrate << '\n' << std::flush;
                    break;
                }
                if (!streamReady) {
                    streamReady = true;
                    std::cout << "STREAM_READY target=" << streamBitrate << " mode=" << (o.bitrate ? "manual" : "auto")
                        << '\n' << std::flush;
                }
                if (adaptBitrate && encoder.hardware()) {
                    const auto sendSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - sendStart).count();
                    const auto decision = automaticBitrate.Observe(80 + packet.config.sequenceHeader.size() + packet.data.size() + packet.audio.samples.size() * sizeof(int16_t),
                        sendSeconds, secondsNow());
                    if (decision.reason != lan3d::media::AdaptiveBitrate::Reason::Hold) bitrateDecision = decision;
                }
                if (!packet.audio.samples.empty()) {
                    ++audioPacketsSent; audioFramesSent += packet.audio.Frames();
                    std::lock_guard lock(audioMeterMutex); sentMeter.Add(packet.audio, GetTickCount64()); sentAudioFrames += packet.audio.Frames();
                }
                ++sent;
                if (o.profile) { ++profile.sent; ++profile.totalSent; }
                if (o.frames && sent >= o.frames) break;
            }
            if (connection.IsOpen() && bitrateDecision.target != streamBitrate) {
                if (encoder.UpdateBitrate(bitrateDecision.target, error)) {
                    streamBitrate = encoder.config().bitrate;
                    std::cout << "AUTO_BITRATE target=" << streamBitrate << " reason="
                        << (bitrateDecision.reason == lan3d::media::AdaptiveBitrate::Reason::Congestion ? "congestion" : "probe")
                        << " observed=" << static_cast<uint64_t>(bitrateDecision.payloadBitsPerSecond)
                        << " send_fraction=" << bitrateDecision.sendFraction << '\n' << std::flush;
                } else {
                    // A rejected reconfiguration leaves the running session intact.
                    // A failure after it changed (e.g. a missing new sequence header)
                    // cannot safely continue with stale stream configuration.
                    if (encoder.config().bitrate != streamBitrate) throw std::runtime_error(error);
                    adaptBitrate = false;
                    automaticBitrate.StartStream(streamFps, streamBitrate, secondsNow());
                    std::cout << "AUTO_BITRATE target=" << streamBitrate << " reason=reconfigure-unavailable detail="
                        << error << '\n' << std::flush;
                }
            }
            if (now - lastStatus > std::chrono::seconds(2)) {
                if (systemAudio) {
                    const auto audioStatus = audioCapture.Status();
                    if (audioStatus != previousAudioStatus) {
                        previousAudioStatus = audioStatus;
                        std::cout << "AUDIO_CAPTURE " << audioStatus << '\n' << std::flush;
                    }
                }
                std::cout << "Sent " << sent << " frames; source sequence " << lastSequence << '\n' << std::flush;
                if (busActive) { const auto status = bus.Status(); if (!status.empty()) std::cout << "Capture: " << status << '\n' << std::flush; }
                lastStatus = now;
            }
        }
        profile.Print(true);
        audioCapture.Stop();
        if (systemAudio || o.testAudio)
            std::cout << "AUDIO_SENT packets=" << audioPacketsSent << " sample_frames=" << audioFramesSent
                << " format=48000/2/s16 source=" << (o.testAudio ? "test-silence" : "system-loopback") << '\n' << std::flush;
        connection.Close();
    }
    std::cout << "Stopped. Sent " << sent << " frames.\n";
    return o.frames && sent < o.frames && !stopping() ? 3 : 0;
} catch (const std::exception& e) {
    std::cerr << "LAN3DSender: " << e.what() << '\n';
    return 1;
}
