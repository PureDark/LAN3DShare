#pragma once
#include "common/Frame.h"
#include "common/GpuFrame.h"
#include "audio/AudioBlock.h"
#include <memory>
#include <string>
#include <vector>

namespace lan3d::media {
// Wire v5 is H.264/HEVC/AV1, BT.709 limited-range NV12, SDR, progressive.
// Each access unit includes the config so a fresh connection can start at an IDR.
inline constexpr uint32_t MaxEncodedBytes = 32u * 1024 * 1024;
inline constexpr uint32_t MaxSequenceHeaderBytes = 64u * 1024;
enum class CodecId : uint32_t { H264 = 0x48323634, Hevc = 0x48455643, Av1 = 0x41563031 };
constexpr uint32_t CodecBit(CodecId codec) {
    return codec == CodecId::H264 ? 1u : codec == CodecId::Hevc ? 2u : codec == CodecId::Av1 ? 4u : 0u;
}
const char* CodecName(CodecId codec);
uint32_t HardwareEncodeCapabilities();
uint32_t HardwareDecodeCapabilities();
struct CodecConfig {
    uint32_t width{}, height{}, fps{}, bitrate{};
    std::vector<uint8_t> sequenceHeader;
    CodecId codec{CodecId::H264};
};
struct EncodedFrame {
    CodecConfig config;
    uint64_t sequence{}, timestamp100ns{};
    bool keyframe{};
    std::vector<uint8_t> data;
    ContentKind kind{ContentKind::LeiaSbs};
    audio::Block audio;
    uint32_t displayWidth{}, displayHeight{}; // Logical single-eye/desktop aspect; zero pair = packed pixels.
};
bool ValidConfig(const CodecConfig& config);
bool ValidPacket(const EncodedFrame& frame);
struct EncodeTiming {
    double inputSubmitMs{}, encodeSubmitMs{}, bitstreamWaitMs{}, totalMs{};
    double flushMs{}, mapInputMs{}, encodePictureMs{};
    double sourceCopyGpuMs{}, destinationCopyGpuMs{};
};

// Objects own COM/MF initialization. Create, use, and destroy on one thread.
// The original Configure overload stays software-only for reproducible tests.
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool Configure(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, std::string& error);
    bool Configure(const CodecConfig& config, bool hardware, std::string& error);
    bool ConfigureGpu(const CodecConfig& config, const GpuFrame& input, std::string& error, const LUID* adapter = nullptr);
    bool Encode(const Frame& frame, std::vector<EncodedFrame>& packets, std::string& error);
    // Successful return completes all reads of the leased source texture.
    bool EncodeGpu(const GpuFrame& frame, std::vector<EncodedFrame>& packets, std::string& error);
    bool GpuPreview(Frame& thumbnail);
    EncodeTiming lastTiming() const;
    bool RequestKeyFrame(std::string& error);
    // Changes only the rate-control target, without recreating GPU resources.
    bool UpdateBitrate(uint32_t bitrate, std::string& error);
    const CodecConfig& config() const;
    bool hardware() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Decoder {
public:
    explicit Decoder(bool preferHardware = false, const LUID* gpuOutputAdapter = nullptr);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    bool Configure(const CodecConfig& config, std::string& error);
    // Reconfigures on stream changes; only a keyframe may start a new stream.
    bool Decode(const EncodedFrame& packet, std::vector<Frame>& frames, std::string& error);
    bool hardware() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
