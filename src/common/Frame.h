#pragma once
#include <cstdint>
#include <vector>
#include <memory>

namespace lan3d {
// V2 image contract: top-down BGRA8, SDR sRGB, full range. Content kind travels
// with every frame; only LeiaSbs contains left/right eye rectangles of width/2.
enum class ContentKind : uint32_t { Desktop = 0, LeiaSbs = 1, LeiaLeft = 2 };
inline bool IsLeia(ContentKind kind) { return kind == ContentKind::LeiaSbs || kind == ContentKind::LeiaLeft; }
inline constexpr uint32_t MaxWidth = 8192;
inline constexpr uint32_t MaxHeight = 4320;
inline constexpr uint32_t MaxFrameBytes = MaxWidth * MaxHeight * 4;
struct Frame {
    uint32_t width{}, height{}, stride{};
    uint64_t sequence{}, timestamp100ns{};
    std::vector<uint8_t> bgra;
    ContentKind kind{ContentKind::LeiaSbs};
    // Logical single-eye dimensions for SBS, whole-image dimensions for desktop.
    // A zero pair derives the display aspect from the packed pixel dimensions.
    uint32_t displayWidth{}, displayHeight{};
    std::shared_ptr<struct GpuImage> gpu;
};
inline bool ValidDisplaySize(uint32_t width, uint32_t height) {
    return (!width && !height) || (width && height && width <= MaxWidth && height <= MaxHeight);
}
inline bool ValidFrame(const Frame& f) {
    return f.width >= 4 && !(f.width & 1) && f.width <= MaxWidth &&
        f.height >= 2 && f.height <= MaxHeight && f.stride >= f.width * 4 &&
        uint64_t(f.stride) * f.height <= MaxFrameBytes &&
        f.bgra.size() >= uint64_t(f.stride) * f.height &&
        (f.kind == ContentKind::Desktop || IsLeia(f.kind)) &&
        ValidDisplaySize(f.displayWidth, f.displayHeight);
}
}
