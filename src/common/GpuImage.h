#pragma once
#include "GpuFrame.h"
#include <memory>
namespace lan3d {
// In-process ownership only; never serialized. A decoder pool may reuse the
// texture only after all frames and renderer GPU-read leases release it.
struct GpuImage {
    GpuFrame metadata;
    virtual ~GpuImage() = default;
};
inline bool ValidVideoFrame(const Frame& f) {
    return ValidFrame(f) || (f.gpu && ValidGpuFrame(f.gpu->metadata) &&
        f.gpu->metadata.width == f.width && f.gpu->metadata.height == f.height &&
        ValidDisplaySize(f.displayWidth, f.displayHeight));
}
}
