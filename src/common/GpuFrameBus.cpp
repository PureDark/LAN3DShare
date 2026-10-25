#include "GpuFrameBus.h"
#include <algorithm>
#include <array>

namespace lan3d {
namespace {
constexpr uint32_t Magic = 0x33475055, Version = 1, SlotCount = 8;
enum : uint32_t { Free, Ready, Held };
struct Slot { GpuFrame frame; uint64_t owner{}; uint32_t state{}; };
struct Header {
    uint32_t magic{}, version{}, bytes{}, count{};
    uint64_t latest{};
    std::array<Slot, SlotCount> slots;
};
struct Lock {
    HANDLE handle{}; bool held{};
    explicit Lock(HANDLE h, DWORD timeout = 0) : handle(h) {
        if (h) { const auto r = WaitForSingleObject(h, timeout); held = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED; }
    }
    ~Lock() { if (held) ReleaseMutex(handle); }
};
}
struct GpuFrameBus::Impl {
    HANDLE mapping{}, mutex{};
    Header* header{};
    uint64_t owner{};
    bool writer{};
    ~Impl() {
        // Destruction does not acknowledge GPU work. Abandoned Held slots are
        // quarantined instead of allowing a producer to overwrite a live read.
        if (header) UnmapViewOfFile(header);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
};
GpuFrameBus::GpuFrameBus() : impl_(std::make_unique<Impl>()) {}
GpuFrameBus::~GpuFrameBus() = default;
void GpuFrameBus::Close() { impl_ = std::make_unique<Impl>(); }
bool GpuFrameBus::Open(uint32_t pid, bool create, std::string& error) {
    auto p = std::make_unique<Impl>();
    const auto name = L"Local\\LAN3DShare.gpu.v1." + std::to_wstring(pid);
    p->mutex = create ? CreateMutexW(nullptr, FALSE, (name + L".lock").c_str())
        : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".lock").c_str());
    if (!p->mutex) { error = "GPU mailbox mutex unavailable"; return false; }
    p->mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Header), name.c_str())
        : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    const bool existing = create && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!p->mapping) { error = "GPU mailbox mapping unavailable"; return false; }
    p->header = static_cast<Header*>(MapViewOfFile(p->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Header)));
    if (!p->header) { error = "Cannot map GPU mailbox"; return false; }
    Lock lock(p->mutex, 1000);
    if (!lock.held) { error = "GPU mailbox initialization timed out"; return false; }
    if (create && !existing) {
        *p->header = {}; p->header->magic = Magic; p->header->version = Version;
        p->header->bytes = sizeof(Header); p->header->count = SlotCount;
    }
    const auto& h = *p->header;
    if (h.magic != Magic || h.version != Version || h.bytes != sizeof(Header) || h.count != SlotCount) {
        error = "Incompatible GPU mailbox layout"; return false;
    }
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    p->owner = (uint64_t(GetCurrentProcessId()) << 32) ^ uint64_t(counter.QuadPart) ^ reinterpret_cast<uintptr_t>(p.get());
    if (!p->owner) p->owner = 1;
    p->writer = create; impl_ = std::move(p); error.clear(); return true;
}
bool GpuFrameBus::Publish(const GpuFrame& frame) {
    auto& p = *impl_;
    if (!p.header || !p.writer || !ValidGpuFrame(frame)) return false;
    Lock lock(p.mutex); if (!lock.held || frame.sequence <= p.header->latest) return false;
    // Replace only unclaimed work. Never let a slow or disconnected receiver
    // stall the game's render thread or accumulate a queue of old frames.
    auto& slots = p.header->slots;
    auto target = std::find_if(slots.begin(), slots.end(), [](const auto& s) { return s.state != Held; });
    if (target == slots.end()) return false;
    for (auto& s : slots) if (s.state == Ready) s = {};
    target->frame = frame; target->owner = 0; target->state = Ready;
    p.header->latest = frame.sequence; return true;
}
bool GpuFrameBus::IsReleased(uint64_t sequence) {
    auto& p = *impl_; if (!p.header) return false;
    Lock lock(p.mutex); if (!lock.held) return false;
    return std::none_of(p.header->slots.begin(), p.header->slots.end(), [&](const auto& s) {
        return s.state != Free && s.frame.sequence == sequence;
    });
}
bool GpuFrameBus::AcquireLatest(GpuFrame& frame, uint64_t afterSequence) {
    auto& p = *impl_; if (!p.header || p.writer) return false;
    Lock lock(p.mutex); if (!lock.held) return false;
    Slot* latest{};
    for (auto& s : p.header->slots) {
        if (s.state == Held && s.owner == p.owner) return false;
        if (s.state == Ready && s.frame.sequence > afterSequence && ValidGpuFrame(s.frame) &&
            (!latest || s.frame.sequence > latest->frame.sequence)) latest = &s;
    }
    if (!latest) return false;
    latest->owner = p.owner; latest->state = Held; frame = latest->frame; return true;
}
bool GpuFrameBus::Release(uint64_t sequence) {
    auto& p = *impl_; if (!p.header || p.writer) return false;
    Lock lock(p.mutex); if (!lock.held) return false;
    for (auto& s : p.header->slots) if (s.state == Held && s.owner == p.owner && s.frame.sequence == sequence) {
        s = {}; return true;
    }
    return false;
}
}
