#pragma once
#include "audio/AudioBlock.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace lan3d::audio::detail {
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    char text[192]{};
    std::snprintf(text, sizeof(text), "%s (0x%08lX)", operation, static_cast<unsigned long>(hr));
    throw std::runtime_error(text);
}
inline uint64_t Now100ns() {
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return uint64_t(value.QuadPart / frequency.QuadPart) * 10000000 +
        uint64_t(value.QuadPart % frequency.QuadPart) * 10000000 / uint64_t(frequency.QuadPart);
}
struct Apartment {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    Apartment() { Check(hr, "COM initialization"); }
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct AudioPriority {
    DWORD task{};
    HANDLE handle{AvSetMmThreadCharacteristicsW(L"Audio", &task)};
    ~AudioPriority() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};
inline std::wstring DeviceId(IMMDevice* device) {
    LPWSTR raw{}; Check(device->GetId(&raw), "Audio endpoint ID");
    const std::wstring id(raw); CoTaskMemFree(raw); return id;
}
struct Endpoint {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    std::wstring id;
    UINT32 bufferFrames{};
    REFERENCE_TIME streamLatency{};
    bool started{};
    void Open(bool capture) {
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)), "Audio enumerator");
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Default system output");
        id = DeviceId(device.Get());
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())), "Audio client");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = Channels; format.nSamplesPerSec = SampleRate;
        format.wBitsPerSample = 16; format.nBlockAlign = Channels * sizeof(int16_t);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST | (capture ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &format, nullptr),
            capture ? "System loopback PCM initialization" : "Playback PCM initialization");
        Check(client->GetBufferSize(&bufferFrames), "Audio buffer size");
        Check(client->GetStreamLatency(&streamLatency), "Audio stream latency");
    }
    bool IsCurrentDefault() {
        ComPtr<IMMDevice> current;
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current), "Default system output");
        return DeviceId(current.Get()) == id;
    }
    void Start() { if (!started) { Check(client->Start(), "Audio start"); started = true; } }
    void Reset() {
        if (started) Check(client->Stop(), "Audio stop");
        started = false; Check(client->Reset(), "Audio reset");
    }
    ~Endpoint() { if (client && started) client->Stop(); }
};
}
