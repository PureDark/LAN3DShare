// Read-only by default. Explicit --reference-tone emits a very quiet known
// signal for at most three seconds. No volume changes or saved audio.
// Build: cl /std:c++20 /EHsc AudioLevelProbe.cpp /link ole32.lib uuid.lib Mmdevapi.lib
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned SampleRate = 48000, Channels = 2;
void Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s failed (0x%08lX)", operation, static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
}
uint64_t Now100ns() {
    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
    return uint64_t(qpc.QuadPart / frequency.QuadPart) * 10000000 +
        uint64_t(qpc.QuadPart % frequency.QuadPart) * 10000000 / uint64_t(frequency.QuadPart);
}
std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back(); return result;
}
WAVEFORMATEX Format() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = Channels;
    format.nSamplesPerSec = SampleRate; format.wBitsPerSample = 16;
    format.nBlockAlign = Channels * sizeof(int16_t);
    format.nAvgBytesPerSec = SampleRate * format.nBlockAlign;
    return format;
}
struct Apartment {
    Apartment() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~Apartment() { CoUninitialize(); }
};
void PrintVolumes(IMMDevice* device) {
    LPWSTR deviceId{}; Check(device->GetId(&deviceId), "GetId");
    std::cout << "ENDPOINT id=" << std::quoted(Utf8(deviceId)); CoTaskMemFree(deviceId);
    ComPtr<IAudioEndpointVolume> volume;
    Check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(volume.GetAddressOf())), "Endpoint volume activation");
    float scalar{}, db{}; BOOL mute{}; DWORD hardware{};
    Check(volume->GetMasterVolumeLevelScalar(&scalar), "Endpoint scalar");
    Check(volume->GetMasterVolumeLevel(&db), "Endpoint dB");
    Check(volume->GetMute(&mute), "Endpoint mute");
    const auto hardwareHr = volume->QueryHardwareSupport(&hardware);
    std::cout << " scalar=" << scalar << " db=" << db << " mute=" << !!mute
        << " hardware_support=" << hardware << " hardware_hr=" << hardwareHr << '\n';
    ComPtr<IAudioSessionManager2> manager;
    Check(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(manager.GetAddressOf())), "Session manager activation");
    ComPtr<IAudioSessionEnumerator> sessions;
    Check(manager->GetSessionEnumerator(&sessions), "Session enumeration");
    int count{}; Check(sessions->GetCount(&count), "Session count");
    for (int index = 0; index < count; ++index) {
        try {
            ComPtr<IAudioSessionControl> control;
            Check(sessions->GetSession(index, &control), "Session");
            ComPtr<IAudioSessionControl2> control2; Check(control.As(&control2), "Session control2");
            ComPtr<ISimpleAudioVolume> sessionVolume; Check(control.As(&sessionVolume), "Session volume");
            DWORD pid{}; Check(control2->GetProcessId(&pid), "Session PID");
            LPWSTR name{}; Check(control->GetDisplayName(&name), "Session name");
            const auto displayName = Utf8(name); CoTaskMemFree(name);
            AudioSessionState state{}; Check(control->GetState(&state), "Session state");
            Check(sessionVolume->GetMasterVolume(&scalar), "Session scalar");
            Check(sessionVolume->GetMute(&mute), "Session mute");
            std::cout << "SESSION pid=" << pid << " name=" << std::quoted(displayName)
                << " scalar=" << scalar << " mute=" << !!mute << " state=" << int(state)
                << " system_sounds=" << (control2->IsSystemSoundsSession() == S_OK) << '\n';
        } catch (const std::exception& error) {
            std::cout << "SESSION_ERROR index=" << index << " message=" << std::quoted(error.what()) << '\n';
        }
    }
}

// All asynchronous state belongs to this COM object, including activation input
// and the event. A timeout never leaves a callback referring to a caller's stack.
class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT variant{};
    HRESULT result{E_PENDING};
    ComPtr<IAudioClient> client;
    Activation(DWORD targetPid, bool include) {
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = targetPid;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = include ?
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        variant.vt = VT_BLOB; variant.blob.cbSize = sizeof(parameters);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    }
    ~Activation() { if (completed) CloseHandle(completed); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activationResult{E_FAIL}; ComPtr<IUnknown> unknown;
        result = operation->GetActivateResult(&activationResult, &unknown);
        if (SUCCEEDED(result)) result = activationResult;
        if (SUCCEEDED(result)) result = unknown.As(&client);
        SetEvent(completed); return S_OK;
    }
};
ComPtr<IAudioClient> OpenProcessLoopback(DWORD targetPid, bool include = false) {
    auto completion = Microsoft::WRL::Make<Activation>(targetPid, include);
    if (!completion || !completion->completed) throw std::runtime_error("Cannot allocate activation completion");
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    Check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &completion->variant, completion.Get(), &operation), "Process loopback activation");
    const DWORD wait = WaitForSingleObject(completion->completed, 5000);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("Process loopback activation timeout/wait failure");
    Check(completion->result, "Process loopback activation result");
    return completion->client;
}
struct Levels {
    uint64_t frames{}, silentFrames{}, packets{}, timestampErrors{}, discontinuities{}, nonzeroSamples{};
    uint64_t firstQpc{}, lastEndQpc{}, firstReadQpc{}, lastReadQpc{};
    long double squareSum{};
    unsigned peak{};
    void Add(const BYTE* data, UINT32 count, DWORD flags, uint64_t qpc, uint64_t readQpc) {
        frames += count; ++packets;
        if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestampErrors;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
        if (qpc && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
            if (!firstQpc) firstQpc = qpc;
            lastEndQpc = qpc + uint64_t(count) * 10000000 / SampleRate;
        }
        if (!firstReadQpc) firstReadQpc = readQpc;
        lastReadQpc = readQpc;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { silentFrames += count; return; }
        if (!data) throw std::runtime_error("Non-silent audio packet has no data");
        const auto samples = reinterpret_cast<const int16_t*>(data);
        for (size_t index = 0; index < size_t(count) * Channels; ++index) {
            const int sample = samples[index];
            peak = std::max(peak, unsigned(std::abs(sample)));
            squareSum += int64_t(sample) * sample;
            nonzeroSamples += sample != 0;
        }
    }
    double Rms() const { return frames ? std::sqrt(double(squareSum / (frames * Channels))) / 32768.0 : 0.0; }
    double Peak() const { return double(peak) / 32768.0; }
    double NonzeroRms() const { return nonzeroSamples ? std::sqrt(double(squareSum / nonzeroSamples)) / 32768.0 : 0.0; }
    static double Db(double amplitude) {
        return amplitude > 0.0 ? 20.0 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
    }
    void Print(const char* kind, const char* source, unsigned second) const {
        std::cout << kind << " source=" << source << " second=" << second << " frames=" << frames
            << " packets=" << packets << " rms=" << Rms() << " rms_dbfs=" << Db(Rms())
            << " peak=" << Peak() << " peak_dbfs=" << Db(Peak()) << " silent_frames=" << silentFrames
            << " nonzero_samples=" << nonzeroSamples << " nonzero_rms=" << NonzeroRms()
            << " qpc_first100ns=" << firstQpc << " qpc_end100ns=" << lastEndQpc
            << " read_first100ns=" << firstReadQpc << " read_last100ns=" << lastReadQpc
            << " timestamp_errors=" << timestampErrors << " discontinuities=" << discontinuities << '\n';
    }
};
struct Capture {
    const char* source;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    Levels interval, total;
    bool started{};
    explicit Capture(const char* value) : source(value) {}
    ~Capture() { if (started) client->Stop(); }
    void Initialize(ComPtr<IAudioClient> value, bool process) {
        client = std::move(value); const auto format = Format();
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | (process ? 0 : AUDCLNT_STREAMFLAGS_NOPERSIST);
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, process ? 0 : 200000, 0, &format, nullptr),
            "Capture PCM initialization");
        Check(client->GetService(IID_PPV_ARGS(&capture)), "Audio capture service");
        UINT32 frames{}; REFERENCE_TIME latency{};
        const auto bufferHr = client->GetBufferSize(&frames), latencyHr = client->GetStreamLatency(&latency);
        std::cout << "CAPTURE_READY source=" << source << " rate=" << SampleRate << " channels=" << Channels
            << " format=s16 buffer_frames=" << frames << " buffer_hr=" << bufferHr
            << " latency100ns=" << latency << " latency_hr=" << latencyHr << '\n';
    }
    void Start() { Check(client->Start(), "Capture start"); started = true; }
    void Drain() {
        if (!started) return;
        UINT32 available{}; Check(capture->GetNextPacketSize(&available), "Next packet");
        while (available) {
            BYTE* data{}; UINT32 frames{}; DWORD flags{}; UINT64 position{}, qpc{};
            Check(capture->GetBuffer(&data, &frames, &flags, &position, &qpc), "Capture buffer");
            if (!frames) break;
            const auto readQpc = Now100ns();
            try { interval.Add(data, frames, flags, qpc, readQpc); total.Add(data, frames, flags, qpc, readQpc); }
            catch (...) { capture->ReleaseBuffer(frames); throw; }
            Check(capture->ReleaseBuffer(frames), "Release capture buffer");
            Check(capture->GetNextPacketSize(&available), "Next packet");
        }
    }
    void PrintInterval(unsigned second) { interval.Print("LEVEL", source, second); interval = {}; }
};
struct ReferenceTone {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames{};
    uint64_t submitted{}, frameLimit{};
    bool started{};
    Levels expected;
    ~ReferenceTone() { Stop(); }
    void Initialize(IMMDevice* device, unsigned seconds) {
        frameLimit = uint64_t(std::min(seconds, 3u)) * SampleRate;
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())), "Reference render client");
        const auto format = Format(); GUID session{};
        Check(CoCreateGuid(&session), "Reference session GUID");
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
            200000, 0, &format, &session), "Reference PCM initialization");
        Check(client->GetBufferSize(&bufferFrames), "Reference buffer size");
        Check(client->GetService(IID_PPV_ARGS(&render)), "Reference render service");
        ComPtr<ISimpleAudioVolume> volume;
        Check(client->GetService(IID_PPV_ARGS(&volume)), "Reference session volume");
        float scalar{}; BOOL mute{};
        Check(volume->GetMasterVolume(&scalar), "Reference session scalar");
        Check(volume->GetMute(&mute), "Reference session mute");
        std::cout << "REFERENCE_SESSION scalar=" << scalar << " mute=" << !!mute << " changed_volume=false\n";
        if (std::abs(scalar - 1.0f) > 0.000001f || mute)
            throw std::runtime_error("Reference session is not unity/unmuted; will not alter it or render");
        std::cout << "REFERENCE_CONFIG frequency_hz=997 nominal_pcm_peak=128 normalized_peak=" << (128.0 / 32768.0)
            << " seconds=" << (double(frameLimit) / SampleRate) << " fade_ms=10\n";
    }
    void Pump() {
        if (submitted == frameLimit) return;
        UINT32 padding{}; Check(client->GetCurrentPadding(&padding), "Reference padding");
        const UINT32 available = padding < bufferFrames ? bufferFrames - padding : 0;
        const auto count = UINT32(std::min<uint64_t>(available, frameLimit - submitted));
        if (!count) return;
        BYTE* data{}; Check(render->GetBuffer(count, &data), "Reference buffer");
        auto samples = reinterpret_cast<int16_t*>(data);
        constexpr double Pi = 3.14159265358979323846;
        constexpr uint64_t FadeFrames = SampleRate / 100;
        for (UINT32 offset = 0; offset < count; ++offset) {
            const auto frame = submitted + offset;
            const auto edgeFrames = std::min(frame, frameLimit - 1 - frame);
            const double fade = std::min(1.0, double(edgeFrames) / FadeFrames);
            const auto sample = int16_t(std::lround(128.0 * fade * std::sin(2.0 * Pi * 997.0 * double(frame) / SampleRate)));
            samples[size_t(offset) * Channels] = sample;
            samples[size_t(offset) * Channels + 1] = sample;
        }
        // Statistics cover exactly the PCM submitted, including both fades.
        expected.Add(data, count, 0, 0, Now100ns());
        Check(render->ReleaseBuffer(count, 0), "Reference release buffer");
        submitted += count;
    }
    void Start() { Pump(); Check(client->Start(), "Reference start"); started = true; }
    void Stop() { if (started) { client->Stop(); started = false; } }
};
}
int main(int argc, char** argv) {
    try {
        bool compare{}, referenceTone{}, customPid{}; unsigned seconds = 5; DWORD excludedPid = GetCurrentProcessId();
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--compare") compare = true;
            else if (argument == "--reference-tone") { referenceTone = true; compare = true; }
            else if (argument == "--seconds" && index + 1 < argc) {
                size_t parsed{}; const std::string raw = argv[++index];
                const auto value = std::stoul(raw, &parsed);
                if (parsed != raw.size() || value < 1 || value > 15) throw std::runtime_error("--seconds must be 1..15");
                seconds = unsigned(value);
            } else if (argument == "--exclude-pid" && index + 1 < argc) {
                size_t parsed{}; const std::string raw = argv[++index];
                const auto value = std::stoul(raw, &parsed);
                if (parsed != raw.size() || !value) throw std::runtime_error("--exclude-pid must be a positive PID");
                excludedPid = DWORD(value); customPid = true;
            } else if (argument == "--help") {
                std::cout << "AudioLevelProbe [--compare] [--seconds 1..15] [--exclude-pid PID]\n"
                    "AudioLevelProbe --reference-tone [--seconds 1..3]\n"
                    "Default: read endpoint/session volumes only. --compare: read both loopbacks; no audio output or files.\n"
                    "--reference-tone: explicitly emit a quiet peak-128 PCM tone (maximum 3s); capture self only for calibration.\n";
                return 0;
            } else throw std::runtime_error("Unknown argument: " + argument);
        }
        if (referenceTone && customPid) throw std::runtime_error("--reference-tone must include self, so --exclude-pid is incompatible");
        if (referenceTone) seconds = std::min(seconds, 3u);
        std::cout << std::setprecision(9) << std::boolalpha;
        Apartment apartment;
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Enumerator");
        ComPtr<IMMDevice> device;
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Default render endpoint");
        PrintVolumes(device.Get());
        if (!compare) return 0;
        Capture endpoint("endpoint"), process(referenceTone ? "process-include-self" : "process-exclude-tree");
        ComPtr<IAudioClient> baseline;
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(baseline.GetAddressOf())), "Endpoint audio client");
        endpoint.Initialize(std::move(baseline), false);
        bool processReady{};
        try { process.Initialize(OpenProcessLoopback(excludedPid, referenceTone), true); processReady = true; }
        catch (const std::exception& error) {
            std::cout << "CAPTURE_ERROR source=" << process.source << " message=" << std::quoted(error.what()) << '\n';
        }
        std::unique_ptr<ReferenceTone> tone;
        if (referenceTone) {
            if (!processReady) throw std::runtime_error("Reference process capture unavailable; no tone will be rendered");
            tone = std::make_unique<ReferenceTone>(); tone->Initialize(device.Get(), seconds);
        }
        std::cout << "COMPARE_BEGIN target_pid=" << excludedPid << " include=" << referenceTone << " seconds=" << seconds
            << " qpc100ns=" << Now100ns() << " records_audio=false changes_volume=false renders_audio=" << referenceTone << '\n' << std::flush;
        endpoint.Start(); if (processReady) process.Start();
        const auto start = Now100ns();
        if (tone) tone->Start();
        const auto stopAt = start + uint64_t(seconds) * 10000000;
        const auto tail = tone ? 2000000u : 0u; // Drain capture latency after the renderer stops.
        unsigned second = 1;
        while (Now100ns() < stopAt + tail) {
            if (tone) { if (Now100ns() >= stopAt) tone->Stop(); else tone->Pump(); }
            endpoint.Drain(); process.Drain();
            if (second <= seconds && Now100ns() >= start + uint64_t(second) * 10000000) {
                endpoint.PrintInterval(second); if (processReady) process.PrintInterval(second);
                std::cout << std::flush; ++second;
            }
            Sleep(1);
        }
        if (tone) tone->Stop();
        endpoint.Drain(); process.Drain();
        if (endpoint.interval.frames) endpoint.PrintInterval(second);
        if (processReady && process.interval.frames) process.PrintInterval(second);
        endpoint.total.Print("TOTAL", endpoint.source, seconds);
        if (processReady) {
            process.total.Print("TOTAL", process.source, seconds);
            const auto ratio = endpoint.total.Rms() > 0 ? process.total.Rms() / endpoint.total.Rms() : 0.0;
            std::cout << "COMPARE_RESULT process_to_endpoint_rms=" << ratio << " difference_db=" << Levels::Db(ratio)
                << " note=aggregate_energy_only_not_waveform_aligned\n";
            if (tone) {
                tone->expected.Print("EXPECTED_PCM", "reference-tone", seconds);
                const auto rmsRatio = tone->expected.Rms() > 0 ? process.total.Rms() / tone->expected.Rms() : 0.0;
                const auto nonzeroRatio = tone->expected.NonzeroRms() > 0 ? process.total.NonzeroRms() / tone->expected.NonzeroRms() : 0.0;
                const auto peakRatio = tone->expected.Peak() > 0 ? process.total.Peak() / tone->expected.Peak() : 0.0;
                std::cout << "REFERENCE_RESULT rms_ratio=" << rmsRatio << " rms_difference_db=" << Levels::Db(rmsRatio)
                    << " peak_ratio=" << peakRatio << " peak_difference_db=" << Levels::Db(peakRatio)
                    << " nonzero_rms_ratio=" << nonzeroRatio << " nonzero_difference_db=" << Levels::Db(nonzeroRatio)
                    << " note=overall_rms_includes_200ms_capture_tail_nonzero_rms_excludes_exact_zeros\n";
            }
        }
        return processReady ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n'; return 1;
    }
}
