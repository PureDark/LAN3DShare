#include "FrameBus.h"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace lan3d {
namespace {
constexpr uint32_t Magic = 0x334E414C, Version = 2;
struct alignas(64) Header {
    uint32_t magic, version, headerBytes, capacity;
    uint32_t width, height, stride, bytes;
    uint64_t sequence, timestamp100ns;
    uint32_t kind;
    char status[768];
};
constexpr size_t MappingBytes = sizeof(Header) + MaxFrameBytes;
struct Lock {
    HANDLE mutex{};
    bool held{};
    explicit Lock(HANDLE m, DWORD timeout = 0) : mutex(m) {
        if (m) { const auto r = WaitForSingleObject(m, timeout); held = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED; }
    }
    ~Lock() { if (held) ReleaseMutex(mutex); }
};
}
struct FrameBus::Impl {
    HANDLE mapping{}, mutex{};
    Header* header{};
    bool writer{};
    ~Impl() {
        if (header) UnmapViewOfFile(header);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
};
FrameBus::FrameBus() : impl_(std::make_unique<Impl>()) {}
FrameBus::~FrameBus() = default;
void FrameBus::Close() { impl_ = std::make_unique<Impl>(); }
bool FrameBus::Open(uint32_t processId, bool create, std::string& error) {
    auto next = std::make_unique<Impl>();
    const auto base = L"Local\\LAN3DShare.v2." + std::to_wstring(processId);
    next->mutex = create ? CreateMutexW(nullptr, FALSE, (base + L".lock").c_str())
        : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (base + L".lock").c_str());
    if (!next->mutex) { error = "Frame bus mutex unavailable (Win32 " + std::to_string(GetLastError()) + ")"; return false; }
    next->mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(MappingBytes), base.c_str()) : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, base.c_str());
    const bool existing = create && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!next->mapping) { error = "Frame bus mapping unavailable (Win32 " + std::to_string(GetLastError()) + ")"; return false; }
    next->header = static_cast<Header*>(MapViewOfFile(next->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, MappingBytes));
    if (!next->header) { error = "Cannot map frame bus (Win32 " + std::to_string(GetLastError()) + ")"; return false; }
    Lock lock(next->mutex, 1000);
    if (!lock.held) { error = "Frame bus initialization timed out"; return false; }
    if (create && !existing) {
        *next->header = {};
        next->header->magic = Magic;
        next->header->version = Version;
        next->header->headerBytes = sizeof(Header);
        next->header->capacity = MaxFrameBytes;
    }
    const auto& h = *next->header;
    if (h.magic != Magic || h.version != Version || h.headerBytes != sizeof(Header) || h.capacity != MaxFrameBytes) {
        error = "Incompatible frame bus layout"; return false;
    }
    next->writer = create;
    impl_ = std::move(next);
    error.clear();
    return true;
}
bool FrameBus::Publish(const Frame& frame) {
    if (!impl_->header || !impl_->writer || !ValidFrame(frame)) return false;
    Lock lock(impl_->mutex);
    if (!lock.held) return false;
    auto& h = *impl_->header;
    const auto bytes = frame.width * 4 * frame.height;
    auto* dst = reinterpret_cast<uint8_t*>(&h + 1);
    for (uint32_t row = 0; row < frame.height; ++row)
        std::memcpy(dst + size_t(row) * frame.width * 4, frame.bgra.data() + size_t(row) * frame.stride, size_t(frame.width) * 4);
    h.width = frame.width; h.height = frame.height; h.stride = frame.width * 4; h.bytes = bytes;
    h.timestamp100ns = frame.timestamp100ns;
    h.kind = static_cast<uint32_t>(frame.kind);
    h.sequence = std::max(h.sequence + 1, frame.sequence);
    return true;
}
bool FrameBus::ReadLatest(Frame& frame, uint64_t afterSequence) {
    if (!impl_->header) return false;
    Lock lock(impl_->mutex);
    if (!lock.held) return false;
    const auto& h = *impl_->header;
    if (!h.sequence || h.sequence <= afterSequence || h.width < 4 || h.width > MaxWidth || (h.width & 1) ||
        h.height < 2 || h.height > MaxHeight || h.stride != h.width * 4 ||
        uint64_t(h.stride) * h.height != h.bytes || h.bytes > MaxFrameBytes || h.kind > 1) return false;
    Frame out;
    out.width = h.width; out.height = h.height; out.stride = h.stride;
    out.sequence = h.sequence; out.timestamp100ns = h.timestamp100ns;
    out.kind = static_cast<ContentKind>(h.kind);
    const auto* src = reinterpret_cast<const uint8_t*>(&h + 1);
    out.bgra.assign(src, src + h.bytes);
    frame = std::move(out);
    return true;
}
void FrameBus::SetStatus(const std::string& text) {
    if (!impl_->header) return;
    Lock lock(impl_->mutex);
    if (!lock.held) return;
    auto& status = impl_->header->status;
    const auto count = std::min(text.size(), sizeof(status) - 1);
    std::memcpy(status, text.data(), count);
    status[count] = '\0';
}
std::string FrameBus::Status() {
    if (!impl_->header) return {};
    Lock lock(impl_->mutex);
    if (!lock.held) return {};
    return {impl_->header->status, strnlen_s(impl_->header->status, sizeof(impl_->header->status))};
}
uint64_t Timestamp100ns() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return uint64_t(counter.QuadPart / frequency.QuadPart) * 10000000 +
        uint64_t(counter.QuadPart % frequency.QuadPart) * 10000000 / uint64_t(frequency.QuadPart);
}
}
