#include "AudioTelemetry.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace lan3d::audio {
namespace {
constexpr uint32_t Magic = 0x414C3344;
struct Data { uint32_t magic{}, bytes{}; uint64_t tick{}; Telemetry value; };
struct Mapping {
    HANDLE file{}, mutex{}; Data* view{};
    ~Mapping() { if (view) UnmapViewOfFile(view); if (file) CloseHandle(file); if (mutex) CloseHandle(mutex); }
    bool Open(uint32_t pid, bool create) {
        const auto name = L"Local\\LAN3DShare.audio.v1." + std::to_wstring(pid);
        mutex = create ? CreateMutexW(nullptr, FALSE, (name + L".lock").c_str()) : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".lock").c_str());
        file = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Data), name.c_str()) : OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mutex || !file) return false;
        view = static_cast<Data*>(MapViewOfFile(file, create ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, sizeof(Data)));
        return view != nullptr;
    }
};
struct Lock {
    HANDLE handle{}; bool held{};
    explicit Lock(HANDLE h) : handle(h) { const auto wait = WaitForSingleObject(h, 0); held = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }
    ~Lock() { if (held) ReleaseMutex(handle); }
};
bool Valid(Level level) { return std::isfinite(level.peak) && std::isfinite(level.rms) && level.peak >= 0 && level.peak <= 1 && level.rms >= 0 && level.rms <= 1 && level.active <= 1; }
}
struct TelemetryPublisher::Impl {
    std::atomic<bool> stop{};
    std::thread thread;
    explicit Impl(std::function<Telemetry()> snapshot) : thread([this, snapshot = std::move(snapshot)] {
        try {
            Mapping mapping; if (!mapping.Open(GetCurrentProcessId(), true)) return;
            while (!stop) {
                const auto value = snapshot();
                { Lock lock(mapping.mutex); if (lock.held) { mapping.view->magic = 0; mapping.view->value = value; mapping.view->tick = GetTickCount64(); mapping.view->bytes = sizeof(Data); mapping.view->magic = Magic; } }
                for (unsigned i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (...) { /* Meter failure must not interrupt the media session. */ }
    }) {}
    ~Impl() { stop = true; if (thread.joinable()) thread.join(); }
};
TelemetryPublisher::TelemetryPublisher(std::function<Telemetry()> snapshot) {
    try { impl_ = std::make_unique<Impl>(std::move(snapshot)); } catch (...) {}
}
TelemetryPublisher::~TelemetryPublisher() = default;
bool ReadTelemetry(uint32_t pid, Telemetry& result) noexcept {
    try {
        if (!pid) return false;
        Mapping mapping; if (!mapping.Open(pid, false)) return false;
        Lock lock(mapping.mutex); if (!lock.held) return false;
        const auto data = *mapping.view; const auto now = GetTickCount64();
        if (data.magic != Magic || data.bytes != sizeof(Data) || data.tick > now || now - data.tick > 600 ||
            !Valid(data.value.input) || !Valid(data.value.output) || data.value.volume > 800) return false;
        result = data.value; result.detail[sizeof(result.detail) - 1] = 0; return true;
    } catch (...) { return false; }
}
}
