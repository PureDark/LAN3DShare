#include "PreviewChannel.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace lan3d::ui {
namespace {
constexpr uint32_t Magic = 0x50524433, Version = 1, MaxSide = 432;
constexpr uint32_t Capacity = MaxSide * MaxSide * 4;
struct alignas(64) Header {
    uint32_t magic, version, headerBytes, capacity;
    uint32_t width, height, stride, bytes;
    uint32_t sourceWidth, sourceHeight, kind, reserved;
    uint64_t sequence, timestamp100ns;
};
static_assert(sizeof(Header) == 64);
constexpr DWORD MappingBytes = sizeof(Header) + Capacity;
std::wstring Name(uint32_t pid) { return L"Local\\LAN3DShare.preview.v1." + std::to_wstring(pid); }
struct Lock {
    HANDLE mutex{}; bool held{};
    explicit Lock(HANDLE handle) : mutex(handle) {
        if (mutex) { const auto wait = WaitForSingleObject(mutex, 0); held = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }
    }
    ~Lock() { if (held) ReleaseMutex(mutex); }
};
struct Mapping {
    HANDLE mapping{}, mutex{};
    Header* view{};
    ~Mapping() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
    bool Open(uint32_t pid, bool create) {
        const auto name = Name(pid);
        mutex = create ? CreateMutexW(nullptr, FALSE, (name + L".lock").c_str())
            : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".lock").c_str());
        if (!mutex) return false;
        mapping = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, MappingBytes, name.c_str())
            : OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mapping) return false;
        view = static_cast<Header*>(MapViewOfFile(mapping, create ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, MappingBytes));
        return view != nullptr;
    }
};
bool Valid(const Header& h) {
    return h.magic == Magic && h.version == Version && h.headerBytes == sizeof(Header) && h.capacity == Capacity &&
        h.width >= 4 && !(h.width & 1) && h.width <= MaxSide && h.height >= 2 && h.height <= MaxSide &&
        h.stride == h.width * 4 && uint64_t(h.stride) * h.height == h.bytes && h.bytes <= Capacity &&
        h.sourceWidth >= 4 && !(h.sourceWidth & 1) && h.sourceWidth <= MaxWidth &&
        h.sourceHeight >= 2 && h.sourceHeight <= MaxHeight && h.kind <= uint32_t(ContentKind::LeiaLeft);
}
}
struct PreviewPublisher::Impl {
    std::unique_ptr<Mapping> mapping;
    ULONGLONG lastAttempt{};
};
PreviewPublisher::PreviewPublisher() = default;
PreviewPublisher::~PreviewPublisher() = default;
void PreviewPublisher::Publish(const Frame& frame) noexcept {
    try {
        if (!ValidFrame(frame)) return;
        if (!impl_) impl_ = std::make_unique<Impl>();
        const auto now = GetTickCount64();
        if (impl_->lastAttempt && now - impl_->lastAttempt < 1000) return;
        impl_->lastAttempt = now;
        if (!impl_->mapping) {
            auto mapping = std::make_unique<Mapping>();
            if (!mapping->Open(GetCurrentProcessId(), true)) return;
            impl_->mapping = std::move(mapping);
        }
        auto& mapping = *impl_->mapping;
        Lock lock(mapping.mutex);
        if (!lock.held) return;
        const double scale = (std::min)(1.0, double(MaxSide) / (std::max)(frame.width, frame.height));
        const uint32_t width = (std::max)(4u, uint32_t(frame.width * scale) & ~1u);
        const uint32_t height = (std::max)(2u, uint32_t(frame.height * scale) & ~1u);
        Header header{Magic, Version, sizeof(Header), Capacity, width, height, width * 4, width * height * 4,
            frame.width, frame.height, uint32_t(frame.kind), 0, frame.sequence, frame.timestamp100ns};
        mapping.view->magic = 0; // A writer terminated mid-copy cannot leave a valid partial frame.
        auto* output = reinterpret_cast<uint8_t*>(mapping.view + 1);
        for (uint32_t y = 0; y < height; ++y) {
            const auto y0 = (std::min)(frame.height - 1, uint32_t((uint64_t(y) * 4 + 1) * frame.height / (height * 4)));
            const auto y1 = (std::min)(frame.height - 1, uint32_t((uint64_t(y) * 4 + 3) * frame.height / (height * 4)));
            const auto* row0 = frame.bgra.data() + size_t(y0) * frame.stride;
            const auto* row1 = frame.bgra.data() + size_t(y1) * frame.stride;
            for (uint32_t x = 0; x < width; ++x) {
                const auto x0 = (std::min)(frame.width - 1, uint32_t((uint64_t(x) * 4 + 1) * frame.width / (width * 4)));
                const auto x1 = (std::min)(frame.width - 1, uint32_t((uint64_t(x) * 4 + 3) * frame.width / (width * 4)));
                auto* pixel = output + size_t(y) * header.stride + x * 4;
                for (unsigned channel = 0; channel < 4; ++channel)
                    pixel[channel] = uint8_t((unsigned(row0[x0 * 4 + channel]) + row0[x1 * 4 + channel] +
                        row1[x0 * 4 + channel] + row1[x1 * 4 + channel] + 2) / 4);
            }
        }
        *mapping.view = header;
    } catch (...) { /* A card preview must never interrupt capture or decoding. */ }
}
bool ReadPreview(uint32_t processId, PreviewImage& image) noexcept {
    try {
        if (!processId) return false;
        Mapping mapping;
        if (!mapping.Open(processId, false)) return false;
        Lock lock(mapping.mutex);
        if (!lock.held) return false;
        const Header header = *mapping.view;
        if (!Valid(header)) return false;
        PreviewImage next;
        next.sourceWidth = header.sourceWidth; next.sourceHeight = header.sourceHeight;
        auto& frame = next.frame;
        frame.width = header.width; frame.height = header.height; frame.stride = header.stride;
        frame.kind = ContentKind(header.kind); frame.sequence = header.sequence; frame.timestamp100ns = header.timestamp100ns;
        const auto* pixels = reinterpret_cast<const uint8_t*>(mapping.view + 1);
        frame.bgra.assign(pixels, pixels + header.bytes);
        image = std::move(next);
        return true;
    } catch (...) { return false; }
}
}
