#pragma once
#include "Frame.h"
#include <windows.h>
#include <dxgiformat.h>
#include <type_traits>

namespace lan3d {
// Metadata only. The producer publishes only after GPU completion and after
// the recording can no longer execute. A consumer must keep its lease until
// every GPU read of this texture has completed. No pixel data crosses the CPU.
// FP16 Leia captures use display-encoded SDR. The desktop capture API explicitly
// negotiates Windows scRGB before returning an FP16 desktop texture.
struct GpuFrame {
    uint32_t width{}, height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    ContentKind kind{ContentKind::LeiaSbs};
    uint64_t sequence{}, timestamp100ns{};
    LUID adapterLuid{};
    wchar_t textureName[160]{};
};
static_assert(std::is_trivially_copyable_v<GpuFrame>);
inline bool ValidGpuFrame(const GpuFrame& f) {
    return f.width >= 4 && !(f.width & 1) && f.width <= MaxWidth &&
        f.height >= 2 && f.height <= MaxHeight && f.sequence && f.timestamp100ns &&
        (f.format == DXGI_FORMAT_B8G8R8A8_UNORM || f.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         f.format == DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        (f.kind == ContentKind::Desktop || IsLeia(f.kind)) &&
        f.textureName[0] && f.textureName[159] == 0;
}
}
