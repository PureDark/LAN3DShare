#include "common/StageProfile.h"
#include "audio/AudioTelemetry.h"
#include "ui/AudioMeterModel.h"
#include "Renderer.h"
#include "app/Workers.h"
#include "app/WorkerEvents.h"
#include "audio/AudioPlayback.h"
#include "media/Codec.h"
#include "net/Transport.h"
#include "ui/ViewerControl.h"
#include "ui/WebViewHost.h"
#include "ui/PreviewChannel.h"
#include "nlohmann/json.hpp"
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace lan3d;
using Clock = std::chrono::steady_clock;
struct Options {
    std::string host;
    uint16_t port{48730};
    std::filesystem::path runtime{L"C:\\Program Files\\LeiaSR\\Platform\\bin"};
    bool preview{}, selfTest{}, listMonitors{}, fullscreen{}, hidden{}, autoDisplay{}, recoverStream{}, noAudio{}, profile{};
    int monitor{-1};
    unsigned frames{}, receiveFrames{}, timeout{10};
    unsigned volumePercent{100};
    std::wstring stopEvent, readyEvent;
    std::string sessionToken, peerName;
    HWND parentWindow{};
    std::filesystem::path uiSmoke;
};
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), count);
    return result;
}
unsigned Number(const wchar_t* text, unsigned minimum, unsigned maximum, const char* name) {
    size_t used{};
    const auto value = std::stoull(text, &used);
    if (used != wcslen(text) || value < minimum || value > maximum) throw std::runtime_error(std::string("Invalid ") + name);
    return unsigned(value);
}
void Usage() {
    std::cout << "LAN3DReceiver --connect IPv4 [--port 48730] [--monitor N] [--fullscreen]\n"
        "              [--runtime-dir DIR | --preview] [--frames N] [--receive-frames N]\n"
        "              [--timeout SECONDS] [--auto-display] [--hidden (preview only)] [--recover-stream] [--no-audio]\n"
        "              [--volume-percent 0..800] (local playback only; adjustable while receiving)\n"
        "LAN3DReceiver --list-monitors\n"
        "LAN3DReceiver --preview --self-test [--frames 90]\n"
        "Fullscreen button/double-click/F11: toggle fullscreen. Esc: restore fullscreen or exit window.\n"
        "--auto-display: windowed 2D; fullscreen Leia SBS only uses the local runtime.\n"
        "--preview is ordinary 2D SBS and never loads or initializes LeiaSR.\n";
}
Options Parse(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&]() { if (++i >= argc) throw std::runtime_error("Missing value for " + Utf8(arg)); return argv[i]; };
        if (arg == L"--connect") o.host = Utf8(value());
        else if (arg == L"--port") o.port = uint16_t(Number(value(), 1, 65535, "port"));
        else if (arg == L"--monitor") o.monitor = int(Number(value(), 0, 128, "monitor"));
        else if (arg == L"--frames") o.frames = Number(value(), 1, 100000000, "frames");
        else if (arg == L"--receive-frames") o.receiveFrames = Number(value(), 1, 100000000, "receive-frames");
        else if (arg == L"--timeout") o.timeout = Number(value(), 1, 3600, "timeout");
        else if (arg == L"--profile") o.profile = true;
        else if (arg == L"--volume-percent") o.volumePercent = Number(value(), 0, 800, "volume-percent");
        else if (arg == L"--runtime-dir") o.runtime = value();
        else if (arg == L"--stop-event") o.stopEvent = value();
        else if (arg == L"--ready-event") o.readyEvent = value();
        else if (arg == L"--session-token") o.sessionToken = Utf8(value());
        else if (arg == L"--peer-name") o.peerName = Utf8(value());
        else if (arg == L"--parent-window") {
            const std::wstring text = value(); size_t used{};
            const auto handle = std::stoull(text, &used);
            if (used != text.size() || text.empty() || text.front() == L'-') throw std::runtime_error("Invalid parent-window");
            o.parentWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
        }
        else if (arg == L"--viewer-ui-smoke") {
            o.uiSmoke = value(); o.autoDisplay = true; o.preview = true; o.selfTest = true;
        }
        else if (arg == L"--auto-display") o.autoDisplay = true;
        else if (arg == L"--recover-stream") o.recoverStream = true;
        else if (arg == L"--no-audio") o.noAudio = true;
        else if (arg == L"--preview") o.preview = true;
        else if (arg == L"--self-test") o.selfTest = true;
        else if (arg == L"--list-monitors") o.listMonitors = true;
        else if (arg == L"--fullscreen") o.fullscreen = true;
        else if (arg == L"--hidden") o.hidden = true;
        else if (arg == L"--help" || arg == L"-h") { Usage(); o.listMonitors = true; }
        else throw std::runtime_error("Unknown option: " + Utf8(arg));
    }
    if (o.selfTest && !o.preview) throw std::runtime_error("--self-test requires --preview; offline tests must not initialize display hardware");
    if (o.hidden && !o.preview && !o.autoDisplay) throw std::runtime_error("--hidden requires --preview or --auto-display; hidden workers always remain 2D");
    if (o.selfTest && !o.host.empty()) throw std::runtime_error("--self-test cannot be combined with --connect");
    if (o.selfTest && o.receiveFrames) throw std::runtime_error("--receive-frames requires a network source, not --self-test");
    if (o.selfTest && !o.frames && o.uiSmoke.empty()) o.frames = 90;
    if (!o.listMonitors && !o.selfTest && o.host.empty()) throw std::runtime_error("--connect IPv4 is required (or use --preview --self-test)");
    if (!o.sessionToken.empty() && !net::ValidSessionToken(o.sessionToken))
        throw std::runtime_error("Session token must contain exactly 32 hexadecimal characters");
    return o;
}
bool RecoveryEnabled(const Options& options) {
    return options.recoverStream || (options.autoDisplay && !options.sessionToken.empty());
}
struct Monitor { HMONITOR handle{}; MONITORINFOEXW info{}; std::wstring friendly; };
BOOL CALLBACK AddMonitor(HMONITOR handle, HDC, LPRECT, LPARAM user) {
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(user);
    Monitor m; m.handle = handle; m.info.cbSize = sizeof(m.info);
    if (GetMonitorInfoW(handle, &m.info)) {
        DISPLAY_DEVICEW display{}; display.cb = sizeof(display);
        if (EnumDisplayDevicesW(m.info.szDevice, 0, &display, 0)) m.friendly = display.DeviceString;
        monitors.push_back(std::move(m));
    }
    return TRUE;
}
std::vector<Monitor> Monitors() {
    std::vector<Monitor> result;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&result));
    UINT pathCount{}, modeCount{};
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT i = 0; i < pathCount; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
                DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
                target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
                if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
                    for (auto& m : result) if (!_wcsicmp(m.info.szDevice, source.viewGdiDeviceName) && *target.monitorFriendlyDeviceName)
                        m.friendly = target.monitorFriendlyDeviceName;
            }
        }
    }
    return result;
}
int SelectMonitor(const std::vector<Monitor>& monitors, int requested) {
    if (monitors.empty()) throw std::runtime_error("No active Windows display");
    if (requested >= 0) {
        if (size_t(requested) >= monitors.size()) throw std::runtime_error("Monitor index is out of range; use --list-monitors");
        return requested;
    }
    for (size_t i = 0; i < monitors.size(); ++i) {
        auto name = monitors[i].friendly;
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (name.find(L"leia") != std::wstring::npos || name.find(L"dimenco") != std::wstring::npos ||
            name.find(L"m27e6v") != std::wstring::npos || name.find(L"27 3d") != std::wstring::npos) return int(i);
    }
    for (size_t i = 0; i < monitors.size(); ++i) if (monitors[i].info.dwFlags & MONITORINFOF_PRIMARY) return int(i);
    return 0;
}
struct WindowState {
    bool fullscreen{}, designed{}, fallback{}, dirty{true}, paintReady{};
    HWND overlay{}, parent{};
    DWORD parentPid{};
    unsigned volumePercent{100};
    std::function<void(unsigned)> volumeChanged;
    ui::WebViewHost* web{};
    HWND fullscreenButton{}, errorLabel{};
    WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
};
bool ValidParent(const WindowState& state) {
    DWORD pid{};
    return state.parent && IsWindow(state.parent) && GetWindowThreadProcessId(state.parent, &pid) && pid == state.parentPid;
}
void SetVolume(HWND window, WindowState& state, unsigned percent, bool notifyParent) {
    if (percent > 800) return;
    state.volumePercent = percent; state.dirty = true;
    if (state.volumeChanged) state.volumeChanged(percent);
    if (notifyParent && ValidParent(state))
        PostMessageW(state.parent, ui::ViewerVolumeChangedMessage(), reinterpret_cast<WPARAM>(window), LPARAM(percent));
}
DWORD ParentProcessId() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W process{sizeof(process)};
    DWORD result{};
    if (Process32FirstW(snapshot, &process)) do {
        if (process.th32ProcessID == GetCurrentProcessId()) { result = process.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &process));
    CloseHandle(snapshot);
    return result;
}
void SetCorners(HWND window, bool fullscreen) {
    const DWORD corner = fullscreen ? 1 : 2; // DWMWCP_DONOTROUND / DWMWCP_ROUND.
    DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
}
void ToggleFullscreen(HWND window, WindowState& state) {
    if (!state.fullscreen) {
        GetWindowPlacement(window, &state.saved);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, (state.designed ? WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX : WS_OVERLAPPEDWINDOW) | WS_VISIBLE);
        SetWindowPlacement(window, &state.saved);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    state.fullscreen = !state.fullscreen;
    state.dirty = true;
    if (state.fullscreen) state.fallback = false;
    if (state.designed) SetCorners(window, state.fullscreen);
    if (state.fullscreenButton) SetWindowTextW(state.fullscreenButton, state.fullscreen ? L"还原窗口" : L"全屏显示");
    if (state.errorLabel && state.fullscreen) ShowWindow(state.errorLabel, SW_HIDE);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && message == ui::ViewerFullscreenMessage()) {
        // The parent passes its own window as wParam. Only the process that
        // created this receiver may activate its viewer through this channel.
        if (reinterpret_cast<HWND>(wparam) == state->parent && ValidParent(*state)) {
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            if (!state->fullscreen) ToggleFullscreen(window, *state);
            SetForegroundWindow(window);
        }
        return 0;
    }
    if (state && message == ui::ViewerVolumeMessage()) {
        if (reinterpret_cast<HWND>(wparam) == state->parent && ValidParent(*state) && lparam >= 0 && lparam <= 800)
            SetVolume(window, *state, unsigned(lparam), false);
        return 0;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        if (state && state->designed) return 0;
        break;
    case WM_NCHITTEST:
        if (state && state->designed && !state->fullscreen) {
            RECT rect{}; GetWindowRect(window, &rect);
            const int border = MulDiv(6, GetDpiForWindow(window), 96);
            const POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            const bool left = p.x < rect.left + border, right = p.x >= rect.right - border;
            const bool top = p.y < rect.top + border, bottom = p.y >= rect.bottom - border;
            if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            return HTCLIENT;
        }
        break;
    case WM_GETMINMAXINFO:
        if (state && state->designed && !state->fullscreen) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize = {MulDiv(480, dpi, 96), MulDiv(294, dpi, 96)};
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (state && (GetKeyState(VK_CONTROL) & 0x8000) && (wparam == VK_UP || wparam == VK_DOWN)) {
            SetVolume(window, *state, unsigned(std::clamp(int(state->volumePercent) + (wparam == VK_UP ? 25 : -25), 0, 800)), true);
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            if (state && state->fullscreen) ToggleFullscreen(window, *state);
            else if (!state || !state->designed) PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wparam == VK_F11 && state && !(lparam & (1LL << 30))) { ToggleFullscreen(window, *state); return 0; }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == 1001 && HIWORD(wparam) == BN_CLICKED && state) { ToggleFullscreen(window, *state); return 0; }
        break;
    case WM_LBUTTONDBLCLK:
        if (state) { ToggleFullscreen(window, *state); return 0; }
        break;
    case WM_SIZE:
        if (state) {
            if (state->overlay) MoveWindow(state->overlay, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
            if (state->web) state->web->Resize();
            state->dirty = true;
            if (state->fullscreenButton) MoveWindow(state->fullscreenButton, (std::max)(0, int(LOWORD(lparam)) - 124), 10, 112, 32, TRUE);
            if (state->errorLabel) MoveWindow(state->errorLabel, 12, 50, (std::max)(1, int(LOWORD(lparam)) - 24), 60, TRUE);
        }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: {
        if (state && state->fullscreen) return 0;
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) { PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
// The embedded frontend emits ASCII action/edge names. Peer-supplied strings
// never enter this command parser and are posted only as JSON string values.
std::string UiField(const std::string& json, const char* field) {
    if (json.size() > 1024) return {};
    const auto value = nlohmann::json::parse(json, nullptr, false);
    return value.is_object() && value.contains(field) && value[field].is_string() ? value[field].get<std::string>() : std::string{};
}
void ViewerAction(HWND window, WindowState& state, const std::string& message) {
    const auto action = UiField(message, "action");
    if (action == "ready") state.dirty = true;
    else if (action == "viewer-ready") state.paintReady = true;
    else if (action == "volume") {
        const auto value = nlohmann::json::parse(message, nullptr, false);
        if (value.is_object() && value.contains("percent") && value["percent"].is_number_integer() &&
            value["percent"] >= 0 && value["percent"] <= 800)
            SetVolume(window, state, value["percent"].get<unsigned>(), true);
    }
    else if (action == "fullscreen") ToggleFullscreen(window, state);
    else if (action == "escape") { if (state.fullscreen) ToggleFullscreen(window, state); }
    else if (action == "close") PostMessageW(window, WM_CLOSE, 0, 0);
    else if (action == "settings") {
        if (ValidParent(state)) {
            AllowSetForegroundWindow(state.parentPid);
            PostMessageW(state.parent, ui::ViewerSettingsMessage(), reinterpret_cast<WPARAM>(window), 0);
        }
    } else if (action == "drag" && !state.fullscreen) {
        const LPARAM position = GetMessagePos(); ReleaseCapture();
        PostMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, position);
    } else if (action == "resize" && !state.fullscreen) {
        const auto edge = UiField(message, "edge");
        const int hit = edge == "left" ? HTLEFT : edge == "right" ? HTRIGHT : edge == "top" ? HTTOP :
            edge == "bottom" ? HTBOTTOM : edge == "top-left" ? HTTOPLEFT : edge == "top-right" ? HTTOPRIGHT :
            edge == "bottom-left" ? HTBOTTOMLEFT : edge == "bottom-right" ? HTBOTTOMRIGHT : HTNOWHERE;
        if (hit != HTNOWHERE) {
            const LPARAM position = GetMessagePos(); ReleaseCapture();
            PostMessageW(window, WM_NCLBUTTONDOWN, hit, position);
        }
    }
}
class NetworkReceiver {
public:
    std::atomic<bool> stop{};
    std::mutex mutex;
    std::shared_ptr<const Frame> latest;
    std::string status{"Connecting"}, error;
    uint64_t decoded{}, latestGeneration{};
    bool ended{}, recovering{}; // Protected by mutex, as are latest/status/statistics.
    Clock::time_point lastFrame{Clock::now()};
    std::thread worker;
    std::mutex connectionMutex;
    std::shared_ptr<net::TcpConnection> activeConnection;
    ~NetworkReceiver() { Stop(); }
    void Presented(uint64_t timestamp100ns, uint64_t generation) {
        if (audioEnabled) playback.Presented(timestamp100ns, generation);
    }
    void SuspendAudio() { if (audioEnabled) playback.Suspend(); }
    void SetVolume(unsigned percent) { playback.SetVolume(percent); }
    audio::Telemetry AudioTelemetry() const {
        const auto stats = playback.Stats(); audio::Telemetry data;
        data.receiver = 1; data.enabled = audioEnabled; data.ready = stats.ready;
        data.suspended = stats.suspended; data.volume = stats.volumePercent;
        data.input = stats.receivedLevel; data.output = stats.outputLevel;
        data.inputFrames = stats.acceptedFrames; data.outputFrames = stats.submittedFrames; data.droppedFrames = stats.droppedFrames;
        const auto detail = AudioStatus(); strncpy_s(data.detail, detail.c_str(), _TRUNCATE); return data;
    }
    std::string AudioStatus() const { return audioEnabled ? playback.Status() : "disabled"; }
    std::string AudioReceived() {
        std::lock_guard lock(audioGenerationMutex);
        return "AUDIO_RECEIVED packets=" + std::to_string(audioPackets.load()) +
            " sample_frames=" + std::to_string(audioSampleFrames.load()) +
            " resets=" + std::to_string(audioGeneration);
    }
    void Stop() {
        stop = true;
        ResetAudio();
        { std::lock_guard lock(connectionMutex); if (activeConnection) activeConnection->Close(); }
        if (worker.joinable()) worker.join();
        playback.Stop();
    }
    void Start(const Options& options, LUID outputAdapter) {
        playback.SetVolume(options.volumePercent);
        audioEnabled = !options.noAudio && !options.selfTest;
        if (audioEnabled) {
            try { playback.Start(); }
            catch (const std::exception& e) {
                audioEnabled = false;
                std::cerr << "AUDIO_UNAVAILABLE " << e.what() << '\n' << std::flush;
            }
        }
        worker = std::thread([this, options, outputAdapter] {
            StageProfile profile(options.profile, "RECEIVE_PROFILE", {"receive_wait", "decode"});
            try {
                const bool recovery = RecoveryEnabled(options);
                auto lastComplete = Clock::now();
                { std::lock_guard lock(mutex); lastFrame = lastComplete; }
                const uint32_t hardwareDecoders = media::HardwareDecodeCapabilities();
                std::string detail, lastFailure;
                uint64_t previousSequence{}, previousTimestamp{};
                bool havePrevious{};
                std::string previousCodec;
                unsigned attempts{};
                const auto failExpired = [&]() {
                    throw std::runtime_error("Stream recovery failed: no complete video frame within 6 seconds" +
                        (lastFailure.empty() ? std::string{} : "; last error: " + lastFailure));
                };
                // A local monotonic clock bounds the whole no-frame interval,
                // including connection, authentication, and keyframe decode.
                // Sender timestamps are metadata, never cross-PC deadlines.
                const auto timeout = [&](uint32_t legacy) -> uint32_t {
                    if (!recovery) return legacy;
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        lastComplete + std::chrono::seconds(6) - Clock::now()).count();
                    if (remaining <= 0) failExpired();
                    return uint32_t((std::min)(int64_t(250), remaining));
                };
                while (!stop) {
                    const uint64_t generation = ResetAudio();
                    auto connection = std::make_shared<net::TcpConnection>();
                    {
                        std::lock_guard lock(connectionMutex);
                        if (stop) break;
                        activeConnection = connection;
                    }
                    bool cleanEnd{};
                    try {
                        // Both decoder and packet state belong to this socket
                        // generation. No partial bytes or reference frames
                        // can leak into the next connection after a timeout.
                        media::Decoder decoder(true, &outputAdapter);
                        if (connection->Connect(options.host, options.port, timeout(3000), detail) != net::IoResult::Ok)
                            throw std::runtime_error("Connect failed: " + detail);
                        if (stop) break;
                        if (!options.sessionToken.empty() && connection->AuthenticateClient(options.sessionToken, timeout(1500), detail) != net::IoResult::Ok)
                            throw std::runtime_error("Session authentication failed: " + detail);
                        if (stop) break;
                        if (connection->SendCapabilities(hardwareDecoders, timeout(1500), detail) != net::IoResult::Ok)
                            throw std::runtime_error("Decoder capability negotiation failed: " + detail);
                        { std::lock_guard lock(mutex); status = attempts ? "Recovering; waiting for a new keyframe" : "Connected; waiting for SBS"; }
                        bool firstPacket = true, resumed = attempts == 0;
                        uint64_t keySequence{};
                        while (!stop) {
                            media::EncodedFrame packet;
                            const auto receiveStart = Clock::now();
                            const auto result = connection->Receive(packet, timeout(250), detail);
                            if (result == net::IoResult::Ok) profile.Record(0, receiveStart);
                            if (stop) break;
                            if (result == net::IoResult::Timeout) continue; // Idle, no partial packet was consumed.
                            if (result == net::IoResult::Closed && !recovery) {
                                ResetAudio();
                                std::lock_guard lock(mutex); ended = true; status = "Sender ended the stream";
                                cleanEnd = true; break;
                            }
                            if (result != net::IoResult::Ok)
                                throw std::runtime_error(result == net::IoResult::Closed ? "Video connection closed" : "Receive failed: " + detail);
                            if (firstPacket) {
                                if (!packet.keyframe) throw std::runtime_error("Reconnected stream did not begin with a keyframe");
                                firstPacket = false; keySequence = packet.sequence;
                            }
                            std::vector<Frame> output;
                            const auto decodeStart = Clock::now();
                            if (!decoder.Decode(packet, output, detail)) throw std::runtime_error("Decode failed: " + detail);
                            profile.Record(1, decodeStart); profile.Print();
                            if (stop) break;
                            if (recovery && Clock::now() >= lastComplete + std::chrono::seconds(6)) failExpired();
                            const auto codec = std::string(decoder.hardware() ? "NVDEC " : "Software ") + media::CodecName(packet.config.codec);
                            if (codec != previousCodec) { previousCodec = codec; std::cout << "DECODER " << codec << '\n' << std::flush; }
                            // Audio can arrive before its decoded image is uploaded,
                            // but playback has no credit until the UI presents it.
                            if (!packet.audio.samples.empty()) {
                                ++audioPackets; audioSampleFrames += packet.audio.Frames();
                            }
                            if (audioEnabled) playback.Push(packet.audio, generation);
                            if (output.empty()) continue;
                            uint64_t unique{};
                            for (const auto& frame : output) {
                                if (!havePrevious || frame.sequence != previousSequence || frame.timestamp100ns != previousTimestamp) ++unique;
                                previousSequence = frame.sequence; previousTimestamp = frame.timestamp100ns; havePrevious = true;
                            }
                            auto frame = std::make_shared<Frame>(std::move(output.back()));
                            if (!ValidVideoFrame(*frame)) throw std::runtime_error("Received invalid SBS dimensions or pixel buffer");
                            lastComplete = Clock::now();
                            uint64_t totalDecoded{};
                            const auto sequence = frame->sequence;
                            {
                                std::lock_guard lock(mutex);
                                latest = std::move(frame); latestGeneration = generation;
                                decoded += unique; totalDecoded = decoded;
                                lastFrame = lastComplete; status = "Receiving"; recovering = false;
                            }
                            if (!resumed) {
                                resumed = true;
                                std::cout << "STREAM_RESUMED attempt=" << attempts << " keyframe=1 key_sequence=" << keySequence
                                    << " decoded_unique=" << totalDecoded << " last_sequence=" << sequence << '\n' << std::flush;
                            }
                        }
                    } catch (const std::exception& e) {
                        ResetAudio();
                        connection->Close();
                        if (stop) break;
                        if (!recovery) throw;
                        lastFailure = e.what();
                        if (Clock::now() >= lastComplete + std::chrono::seconds(6)) failExpired();
                        ++attempts;
                        { std::lock_guard lock(mutex); recovering = true; status = "Recovering video connection"; }
                        std::cout << "STREAM_RECOVERY attempt=" << attempts << " reason=" << lastFailure << '\n' << std::flush;
                    }
                    connection->Close();
                    if (stop || cleanEnd) break;
                    for (unsigned i = 0; i < 5 && !stop; ++i) {
                        if (recovery && Clock::now() >= lastComplete + std::chrono::seconds(6)) failExpired();
                        Sleep(10);
                    }
                }
            } catch (const std::exception& e) {
                ResetAudio();
                std::lock_guard lock(mutex); if (!stop) error = e.what();
            } catch (...) {
                ResetAudio();
                std::lock_guard lock(mutex); if (!stop) error = "Receiver worker failed with an unknown exception";
            }
            ResetAudio();
            std::lock_guard lock(connectionMutex);
            if (activeConnection) activeConnection->Close();
            activeConnection.reset();
        });
    }
private:
    uint64_t ResetAudio() {
        // Serialize invalidation with Stop so a late network reset cannot
        // restore an older generation after the UI has been disconnected.
        std::lock_guard lock(audioGenerationMutex);
        const auto generation = ++audioGeneration;
        if (audioEnabled) playback.Reset(generation);
        return generation;
    }
    audio::Playback playback;
    bool audioEnabled{};
    std::mutex audioGenerationMutex;
    uint64_t audioGeneration{};
    std::atomic<uint64_t> audioPackets{}, audioSampleFrames{};
};
Frame Pattern(unsigned index) {
    // Size changes exercise texture recreation. Mid-tone channels verify the
    // hardware sRGB decode/encode round-trip, not just saturated primary colors.
    Frame f;
    f.width = index < 30 ? 640 : 800; f.height = index < 30 ? 360 : 450;
    f.stride = f.width * 4; f.sequence = index; f.bgra.resize(size_t(f.stride) * f.height);
    for (unsigned y = 0; y < f.height; ++y) for (unsigned x = 0; x < f.width; ++x) {
        auto* pixel = f.bgra.data() + size_t(y) * f.stride + size_t(x) * 4;
        const bool right = x >= f.width / 2;
        pixel[0] = right ? 193 : 37; pixel[1] = right ? 71 : 139; pixel[2] = right ? 43 : 217; pixel[3] = 255;
    }
    return f;
}
std::filesystem::path ViewerDataDirectory() {
    wchar_t local[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!count || count >= MAX_PATH) throw std::runtime_error("Cannot locate the local WebView2 data directory");
    return std::filesystem::path(local) / L"LAN3DShare" / L"WebView2-Viewer";
}
struct ComApartment {
    HRESULT result{E_FAIL};
    explicit ComApartment(bool enabled) {
        if (enabled) {
            result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(result)) throw std::runtime_error("Cannot initialize the viewer UI apartment");
        }
    }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void VerifyTransparentVideo(HWND window) {
    // Sample only this test window's three known interior pixels. No desktop
    // image is copied, retained, or saved. This checks DWM's composed result,
    // which a WebView-only CapturePreview cannot validate.
    DwmFlush();
    RECT rect{}; GetClientRect(window, &rect);
    const POINT positions[]{{rect.right / 4, rect.bottom / 2}, {rect.right / 3, rect.bottom / 2},
        {rect.right * 3 / 4, rect.bottom / 2}};
    const COLORREF expected[]{RGB(217, 139, 37), RGB(217, 139, 37), RGB(43, 71, 193)};
    HDC screen = GetDC(nullptr);
    if (!screen) throw std::runtime_error("Cannot sample the viewer's composed pixels");
    struct ScreenCleanup { HDC value; ~ScreenCleanup() { ReleaseDC(nullptr, value); } } cleanup{screen};
    for (size_t i = 0; i < std::size(positions); ++i) {
        POINT point = positions[i]; ClientToScreen(window, &point);
        if (GetAncestor(WindowFromPoint(point), GA_ROOT) != window)
            throw std::runtime_error("Another window obscures the receiver transparency probe");
        const auto actual = GetPixel(screen, point.x, point.y);
        if (actual == CLR_INVALID || abs(int(GetRValue(actual)) - int(GetRValue(expected[i]))) > 6 ||
            abs(int(GetGValue(actual)) - int(GetGValue(expected[i]))) > 6 ||
            abs(int(GetBValue(actual)) - int(GetBValue(expected[i]))) > 6)
            throw std::runtime_error("D3D video is obscured by the viewer overlay: probe " + std::to_string(i) +
                " RGB=" + std::to_string(GetRValue(actual)) + "," + std::to_string(GetGValue(actual)) + "," +
                std::to_string(GetBValue(actual)));
    }
}
}

int lan3d::RunReceiver(int argc, wchar_t** argv) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        const Options options = Parse(argc, argv);
        WorkerEvent stopEvent(options.stopEvent, SYNCHRONIZE);
        WorkerEvent readyEvent(options.readyEvent, EVENT_MODIFY_STATE);
        if (stopEvent.IsSet()) return 0;
        const bool uiSmoke = !options.uiSmoke.empty();
        const bool designed = options.autoDisplay && !options.hidden && (!options.selfTest || uiSmoke);
        ComApartment apartment(designed);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto monitors = Monitors();
        for (size_t i = 0; i < monitors.size(); ++i) {
            const auto& m = monitors[i];
            std::cout << "Monitor " << i << ": " << Utf8(m.info.szDevice) << " " << Utf8(m.friendly)
                << " " << m.info.rcMonitor.right - m.info.rcMonitor.left << "x" << m.info.rcMonitor.bottom - m.info.rcMonitor.top
                << ((m.info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
        }
        if (options.listMonitors) return 0;
        const auto& monitor = monitors[SelectMonitor(monitors, options.monitor)];
        WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.style = CS_DBLCLKS;
        cls.lpszClassName = ui::ViewerWindowClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101));
        if (!RegisterClassW(&cls)) throw std::runtime_error("Cannot register receiver window class");
        WindowState state; state.designed = designed; state.volumePercent = options.volumePercent;
        if (options.parentWindow) {
            state.parent = options.parentWindow; state.parentPid = ParentProcessId();
            if (!state.parentPid || !ValidParent(state)) throw std::runtime_error("Receiver parent window does not belong to the creating process");
        }
        const RECT area = monitor.info.rcWork;
        UINT dpiX{96}, dpiY{96};
        if (FAILED(GetDpiForMonitor(monitor.handle, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = dpiY = 96;
        const int width = (std::min)(designed ? LONG(MulDiv(720, dpiX, 96)) : options.autoDisplay ? 960L : 1280L, area.right - area.left),
            height = (std::min)(designed ? LONG(MulDiv(440, dpiY, 96)) : options.autoDisplay ? 600L : 720L, area.bottom - area.top);
        const auto displayName = options.peerName.empty() ? std::string("LAN3DShare") : options.peerName;
        const auto initialTitle = Wide(options.autoDisplay ? displayName + " | 等待画面" :
            "LAN3DShare" + (options.peerName.empty() ? std::string{} : " - " + options.peerName));
        HWND window = CreateWindowExW(0, cls.lpszClassName, initialTitle.c_str(),
            designed ? WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX : WS_OVERLAPPEDWINDOW,
            area.left + 20, area.top + 20, width, height, nullptr, nullptr, cls.hInstance, &state);
        if (!window) throw std::runtime_error("Cannot create receiver window");
        struct WindowCleanup { HWND window; ~WindowCleanup() { if (IsWindow(window)) DestroyWindow(window); } } cleanup{window};
        if (designed) SetCorners(window, false);
        else {
            state.fullscreenButton = CreateWindowExW(0, L"BUTTON", L"全屏显示", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                width - 140, 10, 112, 32, window, reinterpret_cast<HMENU>(1001), cls.hInstance, nullptr);
            state.errorLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
                12, 50, width - 40, 60, window, nullptr, cls.hInstance, nullptr);
        }
        receiver::Renderer renderer;
        renderer.Open(window, monitor.handle, options.preview, options.runtime, options.autoDisplay);
        ui::WebViewHost web;
        std::string uiError;
        struct WebPointerCleanup { WindowState& state; ~WebPointerCleanup() { state.web = nullptr; } } webPointerCleanup{state};
        if (designed) {
            WNDCLASSW overlayClass{};
            overlayClass.lpfnWndProc = OverlayProc; overlayClass.hInstance = cls.hInstance;
            overlayClass.lpszClassName = L"LAN3DViewerOverlayWindow"; overlayClass.hCursor = cls.hCursor;
            if (!RegisterClassW(&overlayClass)) throw std::runtime_error("Cannot register the receiver overlay window");
            state.overlay = CreateWindowExW(0, overlayClass.lpszClassName, L"", WS_CHILD, 0, 0, width, height, window, nullptr, cls.hInstance, nullptr);
            if (!state.overlay) throw std::runtime_error("Cannot create the receiver UI overlay");
            state.web = &web;
            if (!web.Open(state.overlay, ViewerDataDirectory(), true,
                [&](const std::string& message) { ViewerAction(window, state, message); },
                [&](const std::string& error) { uiError = error; }))
                throw std::runtime_error(uiError.empty() ? "Cannot open the receiver WebView2 interface" : uiError);
        }
        if ((!options.selfTest || uiSmoke) && !options.hidden) ShowWindow(window, SW_SHOW);
        if (options.fullscreen && !options.selfTest && !options.hidden) ToggleFullscreen(window, state);
        NetworkReceiver network;
        state.volumeChanged = [&](unsigned percent) { network.SetVolume(percent); };
        struct VolumeCleanup { WindowState& state; ~VolumeCleanup() { state.volumeChanged = {}; } } volumeCleanup{state};
        ui::PreviewPublisher preview;
        if (!options.selfTest) network.Start(options, renderer.AdapterLuid());
        audio::TelemetryPublisher audioTelemetry([&] { return network.AudioTelemetry(); });
        nlohmann::json audioModel; uint64_t audioModelTick{};
        readyEvent.Signal();
        std::shared_ptr<const Frame> displayed;
        unsigned presented{}, uploads{}, fpsFrames{};
        uint64_t statsDecoded{}, displayedGeneration{};
        auto lastStats = Clock::now();
        const auto uiStarted = Clock::now();
        auto smokeChanged = Clock::now();
        unsigned smokePhase{};
        bool captureDone{}, captureOk{}, smokePassed{}, overlayVisible{};
        StageProfile displayProfile(options.profile, "DISPLAY_PROFILE", {"upload", "preview", "draw_present"});
        int receiveFps{};
        std::string lastModel;
        bool quit{};
        while (!quit) {
            if (stopEvent.IsSet()) break;
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { quit = true; break; }
                if (message.message == WM_KEYDOWN && message.hwnd != window && IsChild(window, message.hwnd) &&
                    (message.wParam == VK_F11 || message.wParam == VK_ESCAPE ||
                        ((GetKeyState(VK_CONTROL) & 0x8000) && (message.wParam == VK_UP || message.wParam == VK_DOWN)))) {
                    SendMessageW(window, message.message, message.wParam, message.lParam);
                    continue;
                }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (quit) break;
            if (!uiError.empty()) throw std::runtime_error("Receiver UI: " + uiError);
            if (designed && (!web.Ready() || !state.paintReady) && Clock::now() - uiStarted > std::chrono::seconds(15))
                throw std::runtime_error("Receiver WebView2 interface did not become ready");
            if (uiSmoke && Clock::now() - uiStarted > std::chrono::seconds(25))
                throw std::runtime_error("Receiver UI smoke test timed out");
            const bool minimized = IsIconic(window) != FALSE;
            std::string status;
            uint64_t decoded{};
            bool ended{}, recovering{};
            if (options.selfTest) {
                renderer.Upload(Pattern(presented)); ++uploads;
                if (!uiSmoke && presented == 45) SetWindowPos(window, nullptr, 0, 0, 1040, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                status = "Offline self-test";
            } else {
                std::shared_ptr<const Frame> incoming;
                uint64_t incomingGeneration{};
                { std::lock_guard lock(network.mutex);
                    if (stopEvent.IsSet()) break;
                    decoded = network.decoded;
                    ended = network.ended;
                    recovering = network.recovering;
                    if (!network.error.empty()) throw std::runtime_error(network.error);
                    if (ended && options.receiveFrames && decoded < options.receiveFrames)
                        throw std::runtime_error("Sender ended before --receive-frames target: decoded " +
                            std::to_string(decoded) + " of " + std::to_string(options.receiveFrames));
                    if (ended && !decoded) throw std::runtime_error("Sender ended before any complete video frame");
                    if (RecoveryEnabled(options) && Clock::now() - network.lastFrame >= std::chrono::seconds(6))
                        throw std::runtime_error("Stream recovery failed: no complete video frame within 6 seconds");
                    if (!RecoveryEnabled(options) && !ended && Clock::now() - network.lastFrame > std::chrono::seconds(options.timeout))
                        throw std::runtime_error("No complete SBS frame for " + std::to_string(options.timeout) + " seconds");
                    incoming = network.latest; incomingGeneration = network.latestGeneration; status = network.status;
                }
                if (incoming && incoming != displayed) {
                    const auto uploadStart = Clock::now();
                    renderer.Upload(*incoming); displayProfile.Record(0, uploadStart); displayed = std::move(incoming);
                    displayedGeneration = incomingGeneration; ++uploads;
                    const auto previewStart = Clock::now(); if (displayed->gpu) { Frame thumbnail; if (renderer.Thumbnail(*displayed, thumbnail)) preview.Publish(thumbnail); } else preview.Publish(*displayed); displayProfile.Record(1, previewStart);
                    if (displayed->kind == ContentKind::Desktop) state.fallback = false;
                }
            }
            if (options.autoDisplay) {
                // Remove all Chromium pixels before the first interwoven
                // frame. Fullscreen weaving always owns the physical client
                // pixels; neither a CSS transform nor an overlay rescales it.
                const bool mayWeave = state.fullscreen && !options.preview && renderer.HasFrame() && renderer.Content() == ContentKind::LeiaSbs;
                if (designed && mayWeave && overlayVisible) {
                    ShowWindow(state.overlay, SW_HIDE); web.Resize(); overlayVisible = false; SetFocus(window);
                }
                std::string error;
                if (!renderer.SetFullscreen(!options.hidden && !minimized && state.fullscreen, error)) {
                    if (state.fullscreen) ToggleFullscreen(window, state);
                    state.fallback = true; state.dirty = true;
                    if (state.errorLabel) {
                        SetWindowTextW(state.errorLabel, L"无法启用 3D，已恢复窗口预览。\n请检查本机 3D 显示设置；详情见日志。");
                        ShowWindow(state.errorLabel, SW_SHOW);
                    }
                    std::cerr << "LeiaSR fullscreen unavailable: " << error << '\n';
                }
            }
            if (GetTickCount64() - audioModelTick >= 100) {
                audioModel = ui::AudioMeterModel(network.AudioTelemetry()); audioModelTick = GetTickCount64();
            }
            if (designed && web.Ready()) {
                nlohmann::json model{{"type", "viewer"}, {"mode", "viewer"}, {"peerName", displayName},
                    {"kind", renderer.Content() == ContentKind::LeiaSbs ? "sbs" : "desktop"},
                    {"hasFrame", renderer.HasFrame()}, {"fullscreen", state.fullscreen}, {"fallback", state.fallback},
                    {"recovering", recovering},
                    {"volumePercent", state.volumePercent}, {"audio", audioModel},
                    {"receivingFps", receiveFps}, {"width", displayed ? displayed->width : 0}, {"height", displayed ? displayed->height : 0}};
                if (uiSmoke) { model["designScene"] = smokePhase < 6; model["uiState"] = state.fallback ? "viewer-fallback" : "viewer"; }
                const auto serialized = model.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                if (state.dirty || lastModel != serialized) { web.Post(serialized); lastModel = serialized; state.dirty = false; }
                const bool visible = state.paintReady && !renderer.IsWeaving() && !minimized;
                if (visible != overlayVisible) {
                    ShowWindow(state.overlay, visible ? SW_SHOW : SW_HIDE); web.Resize(); overlayVisible = visible;
                    if (!visible) SetFocus(window);
                }
            }
            if (minimized) {
                network.SuspendAudio();
                if (ended && !options.frames) break;
                Sleep(20); continue;
            }
            const auto drawStart = Clock::now();
            const bool ready = renderer.Draw(options.selfTest && (presented == 0 || presented == 30 || presented == 45 || presented + 1 == options.frames));
            displayProfile.Record(2, drawStart); displayProfile.Print();
            if (displayed && !options.hidden && renderer.LastPresentSucceeded())
                network.Presented(displayed->timestamp100ns, displayedGeneration);
            else network.SuspendAudio();
            if (renderer.HasFrame()) { ++presented; ++fpsFrames; }
            if (options.frames && presented >= options.frames) break;
            if (options.receiveFrames && decoded >= options.receiveFrames) break;
            if (ended && !options.frames) break;
            const auto now = Clock::now();
            if (uiSmoke && web.Ready() && state.paintReady) {
                if (!smokePhase && now - uiStarted > std::chrono::milliseconds(700)) {
                    smokePhase = 1; captureDone = false;
                    web.Capture(options.uiSmoke, [&](bool ok) { captureOk = ok; captureDone = true; });
                } else if (smokePhase == 1 && captureDone) {
                    if (!captureOk) throw std::runtime_error("Cannot capture the viewer interface");
                    smokePhase = 10;
                    web.Execute(L"const slider=document.getElementById('viewer-volume'); slider.value=500; slider.dispatchEvent(new Event('input',{bubbles:true}));");
                } else if (smokePhase == 10 && state.volumePercent == 500) {
                    ViewerAction(window, state, R"({"action":"volume","percent":801})");
                    ViewerAction(window, state, R"({"action":"volume","percent":-1})");
                    ViewerAction(window, state, R"({"action":"volume","percent":12.5})");
                    if (state.volumePercent != 500) throw std::runtime_error("Invalid viewer volume was accepted");
                    smokePhase = 2;
                    web.Execute(L"chrome.webview.postMessage({action:'fullscreen'})");
                } else if (smokePhase == 2 && state.fullscreen) {
                    RECT client{}; GetClientRect(window, &client);
                    MONITORINFO active{sizeof(active)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &active);
                    if (client.right != active.rcMonitor.right - active.rcMonitor.left ||
                        client.bottom != active.rcMonitor.bottom - active.rcMonitor.top || renderer.IsWeaving())
                        throw std::runtime_error("Preview fullscreen must cover physical monitor pixels without loading LeiaSR");
                    smokePhase = 11;
                    web.Execute(L"document.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',ctrlKey:true,bubbles:true}));");
                } else if (smokePhase == 11 && state.volumePercent == 525) {
                    smokePhase = 3; web.Execute(L"chrome.webview.postMessage({action:'escape'})");
                } else if (smokePhase == 3 && !state.fullscreen) {
                    state.fallback = true; state.dirty = true; smokeChanged = now; smokePhase = 4;
                } else if (smokePhase == 4 && now - smokeChanged > std::chrono::milliseconds(500)) {
                    smokePhase = 5; captureDone = false;
                    auto fallbackPath = options.uiSmoke;
                    fallbackPath.replace_filename(fallbackPath.stem().wstring() + L"-fallback.png");
                    web.Capture(fallbackPath, [&](bool ok) { captureOk = ok; captureDone = true; });
                } else if (smokePhase == 5 && captureDone) {
                    if (!captureOk) throw std::runtime_error("Cannot capture the viewer fallback interface");
                    smokePhase = 6; state.fallback = false; state.dirty = true; smokeChanged = now;
                    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                } else if (smokePhase == 6 && now - smokeChanged > std::chrono::milliseconds(600)) {
                    VerifyTransparentVideo(window);
                    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    smokePhase = 7; smokePassed = true;
                    web.Execute(L"chrome.webview.postMessage({action:'close'})");
                }
            }
            if (now - lastStats >= std::chrono::seconds(1)) {
                const auto seconds = std::chrono::duration<double>(now - lastStats).count();
                const auto fps = fpsFrames / seconds;
                const auto receivedFps = (decoded - statsDecoded) / seconds;
                receiveFps = int(receivedFps + .5);
                std::string title = "LAN3DShare" + (options.peerName.empty() ? std::string{} : " - " + options.peerName) + " | " + renderer.Mode() + " | " + status +
                    " | display " + std::to_string(int(fps + .5)) + " fps | receive " + std::to_string(int(receivedFps + .5)) + " fps | decoded " + std::to_string(decoded);
                if (renderer.HasFrame() && !ready) title += " | LeiaSR is not ready to weave";
                if (options.autoDisplay) {
                    const std::string mode = !renderer.HasFrame() ? "等待画面" :
                        renderer.IsWeaving() ? (ready ? "LeiaSR 3D" : "3D 画面 · 准备中") :
                        renderer.Content() == ContentKind::LeiaSbs ? "3D 画面 · 窗口预览" : "普通画面";
                    const auto caption = displayName + " | " + mode + " | 接收 " + std::to_string(int(receivedFps + .5)) + " 帧/秒";
                    SetWindowTextW(window, Wide(caption).c_str());
                } else SetWindowTextW(window, Wide(title).c_str());
                std::cout << title << "\n";
                if (!options.selfTest) std::cout << "AUDIO_STATE " << network.AudioStatus() << '\n';
                if (options.autoDisplay) {
                    const nlohmann::json stats{{"kind", renderer.Content() == ContentKind::LeiaSbs ? "sbs" : "desktop"},
                        {"hasFrame", renderer.HasFrame()}, {"receivingFps", receiveFps},
                        {"recovering", recovering},
                        {"width", displayed ? displayed->width : 0}, {"height", displayed ? displayed->height : 0},
                        {"fullscreen", state.fullscreen}, {"weaving", renderer.IsWeaving()}};
                    std::cout << "VIEWER_STATE " << stats.dump() << '\n' << std::flush;
                }
                lastStats = now; fpsFrames = 0; statsDecoded = decoded;
            }
        }
        network.Stop();
        if (!options.selfTest) std::cout << network.AudioReceived() << '\n' << std::flush;
        web.Close(); state.web = nullptr;
        if (uiSmoke && !smokePassed) throw std::runtime_error("Receiver UI smoke test ended before completion");
        if (uiSmoke) std::cout << "Receiver designed UI smoke PASS: viewer/fallback capture, live slider volume=500, invalid volume rejected, fullscreen keyboard volume=525, escape, transparent D3D video, close; no network or LeiaSR\n" << std::flush;
        uint64_t decoded{}, lastSequence{};
        { std::lock_guard lock(network.mutex); decoded = network.decoded; if (network.latest) lastSequence = network.latest->sequence; }
        std::cout << (options.selfTest ? "Receiver preview self-test PASS" : "Receiver stopped")
            << ": decoded_unique=" << decoded << " last_sequence=" << lastSequence
            << " uploaded=" << uploads << " presented=" << presented << "\n";
        if (options.receiveFrames && decoded < options.receiveFrames)
            throw std::runtime_error("Receiver stopped before --receive-frames target: decoded " +
                std::to_string(decoded) + " of " + std::to_string(options.receiveFrames));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "LAN3DReceiver ERROR: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "LAN3DReceiver ERROR: LeiaSR/runtime exception without a standard message\n";
        return 1;
    }
}
