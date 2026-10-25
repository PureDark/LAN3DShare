#pragma once
#include "media/Codec.h"

namespace lan3d::media {
// Driver-only NVDEC. Production output stays on GPU: CUDA/D3D11 interop
// transfers NV12, then DirectX compute converts color. CPU output is diagnostic.
class NvDecoder {
public:
    explicit NvDecoder(const LUID* gpuOutputAdapter = nullptr);
    ~NvDecoder();
    NvDecoder(const NvDecoder&) = delete;
    NvDecoder& operator=(const NvDecoder&) = delete;
    static uint32_t Probe();
    bool Configure(const CodecConfig& config, std::string& error);
    bool Decode(const EncodedFrame& packet, std::vector<Frame>& frames, std::string& error);
private:
    bool gpuOutput_{};
    LUID outputAdapter_{};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
