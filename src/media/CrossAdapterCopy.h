#pragma once
#include "common/GpuFrame.h"
#include <d3d11.h>
#include <memory>
#include <string>

namespace lan3d::media {
// D3D12 COPY queues on both adapters. The only cross-adapter allocation is
// a row-pitched shared buffer; no image is mapped or copied by the CPU.
class CrossAdapterCopy {
public:
    CrossAdapterCopy();
    ~CrossAdapterCopy();
    bool Open(const GpuFrame& source, ID3D11Device* destination, std::string& error);
    // Queues copies and a GPU wait on the destination immediate context.
    // Caller retains source through completed destination reads (NVENC),
    // and finishes those reads before the next Copy. Worker-local.
    struct Timing { double sourceMs{}, destinationMs{}; };
    Timing CompletedTiming() const;
    bool Copy(const GpuFrame& source, ID3D11Texture2D** output, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
