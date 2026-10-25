#pragma once
#include "media/Codec.h"
#include "common/GpuFrame.h"
#include <memory>

namespace lan3d::media {
// NVIDIA driver API; CPU upload or leased shared texture -> D3D11 VP -> NVENC.
// Configure, Encode and destruction must run on the same worker thread.
class NvEncoder {
public:
    NvEncoder();
    ~NvEncoder();
    NvEncoder(const NvEncoder&) = delete;
    NvEncoder& operator=(const NvEncoder&) = delete;
    static uint32_t Probe();
    bool Configure(const CodecConfig& config, std::string& error);
    bool ConfigureGpu(const CodecConfig& config, const GpuFrame& input, std::string& error, const LUID* adapter = nullptr);
    bool Encode(const Frame& frame, std::vector<EncodedFrame>& packets, std::string& error);
    // The caller holds the shared texture lease through a successful return.
    // On failure, retain the lease: submitted GPU reads may still be pending.
    bool EncodeGpu(const GpuFrame& frame, std::vector<EncodedFrame>& packets, std::string& error);
    // Best effort, nonblocking thumbnail. Call regularly; GPU updates are 1 Hz.
    bool GpuPreview(Frame& thumbnail);
    EncodeTiming lastTiming() const;
    bool RequestKeyFrame(std::string& error);
    bool UpdateBitrate(uint32_t bitrate, std::string& error);
    const CodecConfig& config() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
