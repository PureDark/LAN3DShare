#pragma once
#include <algorithm>
#include <cstdint>

namespace lan3d {
struct DesktopRegion { uint32_t x{}, y{}, width{}, height{}; };
inline DesktopRegion SelectDesktopRegion(uint32_t width, uint32_t height, bool center4k) {
    const auto w = center4k ? std::min(width, 3840u) : width;
    const auto h = center4k ? std::min(height, 2160u) : height;
    return {(width - w) / 2, (height - h) / 2, w, h};
}
}
