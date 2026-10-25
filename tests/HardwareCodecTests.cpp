#include "media/Codec.h"
#include "media/NvDecoder.h"
#include "media/NvEncoder.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace lan3d;
using namespace lan3d::media;
namespace {
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void CheckInvalidContract() {
    // These must fail validation before either backend loads a GPU driver.
    const CodecConfig valid{640, 360, 60, 20000000, {}, CodecId::H264};
    auto reject = [](const CodecConfig& config) {
        NvEncoder encoder;
        NvDecoder decoder;
        std::string error;
        Require(!encoder.Configure(config, error) && !error.empty(), "NVENC accepted an invalid configuration");
        error.clear();
        Require(!decoder.Configure(config, error) && !error.empty(), "NVDEC accepted an invalid configuration");
    };
    auto config = valid; config.codec = static_cast<CodecId>(0xDEADBEEF); reject(config);
    config = valid; config.width = 641; reject(config);
    config = valid; config.width = 4098; reject(config);
    config = valid; config.height = 2305; reject(config);
    config = valid; config.fps = 0; reject(config);
    config = valid; config.bitrate = 1000000001; reject(config);
    config = valid; config.bitrate = 1000000000;
    Require(ValidConfig(config), "The 1000 Mbps protocol boundary was rejected");
    config = valid; config.sequenceHeader.resize(MaxSequenceHeaderBytes + 1); reject(config);
    NvDecoder decoder;
    EncodedFrame packet;
    packet.config = valid; packet.data = {1}; packet.keyframe = true;
    packet.kind = static_cast<ContentKind>(3);
    std::vector<Frame> frames;
    std::string error;
    Require(!decoder.Decode(packet, frames, error) && frames.empty() && !error.empty(), "NVDEC accepted an invalid content kind");
    packet.kind = ContentKind::LeiaSbs; packet.keyframe = false;
    Require(!decoder.Decode(packet, frames, error) && frames.empty() && !error.empty(), "NVDEC accepted a new stream without a keyframe");
    packet.keyframe = true; packet.data.clear();
    Require(!decoder.Decode(packet, frames, error) && frames.empty() && !error.empty(), "NVDEC accepted an empty access unit");
    std::cout << "Invalid codec/configuration, content kind, empty packet and fresh non-keyframe rejection PASS (no GPU required)\n";
}
Frame TestFrame(uint32_t width, uint32_t height, unsigned index) {
    Frame frame;
    frame.width = width; frame.height = height; frame.stride = width * 4 + 32;
    constexpr uint64_t sequences[]{0x100000004ull, 0x100000004ull, 0x100000001ull, 0x100000000ull};
    constexpr uint64_t timestamps[]{0x200000500ull, 0x200000500ull, 0x200000100ull, 0x200000300ull};
    frame.sequence = sequences[index % 4]; frame.timestamp100ns = timestamps[index % 4];
    frame.kind = index % 3 == 1 ? ContentKind::Desktop : ContentKind::LeiaSbs;
    frame.displayWidth = index % 3 ? width : 0; frame.displayHeight = index % 3 ? height : 0;
    frame.bgra.assign(size_t(frame.stride) * height, 0xCD);
    constexpr std::array<std::array<uint8_t, 3>, 6> colors{{
        {20, 30, 220}, {230, 60, 15}, {64, 128, 192}, {180, 120, 70}, {255, 255, 255}, {0, 0, 0}
    }};
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const unsigned row = y < height / 3 ? 0 : y < height * 2 / 3 ? 1 : 2;
        const auto& color = colors[row * 2 + (x >= width / 2)];
        auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + x * 4;
        pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2]; pixel[3] = 255;
    }
    return frame;
}
void CheckFrame(const Frame& actual, const Frame& expected) {
    Require(ValidFrame(actual), "Decoded frame is invalid");
    Require(actual.width == expected.width && actual.height == expected.height, "Visible dimensions changed");
    Require(actual.sequence == expected.sequence && actual.timestamp100ns == expected.timestamp100ns && actual.kind == expected.kind,
        "Repeated/non-monotonic metadata or 2D/SBS kind changed");
    Require(actual.displayWidth == expected.displayWidth && actual.displayHeight == expected.displayHeight,
        "Repeated sequence/timestamp confused the logical display aspect");
    for (uint32_t y : {expected.height / 6, expected.height / 2, expected.height * 5 / 6, expected.height - 2}) {
        for (uint32_t x : {expected.width / 4, expected.width * 3 / 4, expected.width - 2}) {
            const auto* a = actual.bgra.data() + size_t(y) * actual.stride + x * 4;
            const auto* b = expected.bgra.data() + size_t(y) * expected.stride + x * 4;
            for (unsigned c = 0; c < 3; ++c) {
                Require(std::abs(int(a[c]) - int(b[c])) <= 8,
                    "BT.709/range/eye-order mismatch at " + std::to_string(x) + "," + std::to_string(y) +
                    " channel " + std::to_string(c) + ": got " + std::to_string(a[c]) + ", expected " + std::to_string(b[c]));
            }
            Require(a[3] == 255, "Decoded alpha is not opaque");
        }
    }
}
EncodedFrame EncodeOne(Encoder& encoder, const Frame& input) {
    std::string error;
    std::vector<EncodedFrame> packets;
    const bool encoded = encoder.Encode(input, packets, error);
    Require(encoded, "Encode: " + error);
    Require(packets.size() == 1, "Low latency hardware encoder must immediately emit exactly one packet");
    const auto& packet = packets.front();
    Require(ValidPacket(packet), "Encoder produced an invalid packet");
    Require(packet.sequence == input.sequence && packet.timestamp100ns == input.timestamp100ns && packet.kind == input.kind,
        "Encoder changed input metadata");
    return std::move(packets.front());
}
void DecodeOne(Decoder& decoder, const EncodedFrame& packet, const Frame& input) {
    std::string error;
    std::vector<Frame> frames;
    const bool decoded = decoder.Decode(packet, frames, error);
    Require(decoded, "Decode: " + error);
    Require(decoder.hardware(), "Hardware decoder silently fell back to software");
    Require(frames.size() == 1, "Low latency hardware decoder must immediately emit exactly one frame");
    CheckFrame(frames.front(), input);
}
void RejectNewStream(const EncodedFrame& packet) {
    Decoder decoder(true);
    std::string error;
    std::vector<Frame> frames;
    Require(!packet.keyframe, "Rejection test needs a non-keyframe");
    Require(!decoder.Decode(packet, frames, error) && frames.empty() && !error.empty(), "Fresh decoder accepted a non-keyframe stream start");
}
void CheckCodec(CodecId codec) {
    Encoder encoder;
    Decoder decoder(true);
    std::string error;
    bool rejectedPredictedFrame{};
    for (const auto dimensions : {std::array<uint32_t, 2>{640, 360}, std::array<uint32_t, 2>{800, 600}, std::array<uint32_t, 2>{650, 359}}) {
        const CodecConfig config{dimensions[0], dimensions[1], 60, 20000000, {}, codec};
        const bool configured = encoder.Configure(config, true, error);
        Require(configured, "Configure: " + error);
        Require(encoder.hardware(), "Hardware encoder silently fell back to software");
        for (unsigned i = 0; i < 8; ++i) {
            if (i == 2 || i == 6) {
                const auto bitrate = i == 2 ? 80000000u : 20000000u;
                const bool updated = encoder.UpdateBitrate(bitrate, error);
                Require(updated, "In-place bitrate update: " + error);
                Require(encoder.config().bitrate == bitrate, "Bitrate update did not reach stream metadata");
            }
            if (i == 4) { const bool requested = encoder.RequestKeyFrame(error); Require(requested, "RequestKeyFrame: " + error); }
            const Frame input = TestFrame(dimensions[0], dimensions[1], i);
            auto packet = EncodeOne(encoder, input);
            // Display aspect is carried by the application protocol, independently
            // of the compressed picture and the hardware encoder's pixel input.
            Require(packet.displayWidth == input.displayWidth && packet.displayHeight == input.displayHeight,
                "Hardware encoder lost logical display dimensions");
            Require(packet.config.codec == codec, "Encoder changed the selected codec");
            Require(packet.config.width == input.width && packet.config.height == input.height, "Packet lost visible dimensions");
            Require(i != 0 || packet.keyframe, "New or resized stream did not start with a keyframe");
            Require((i != 2 && i != 6) || packet.keyframe, "Bitrate update did not refresh the keyframe");
            if (i == 3) {
                Require(!packet.keyframe, "Metadata-only bitrate test requires a predicted frame");
                packet.config.bitrate += 1000000;
            }
            DecodeOne(decoder, packet, input);
            if (i == 4) {
                Require(packet.keyframe, "Requested keyframe was not generated");
                Decoder fresh(true);
                DecodeOne(fresh, packet, input);
            }
            if (!packet.keyframe && !rejectedPredictedFrame) { RejectNewStream(packet); rejectedPredictedFrame = true; }
            if (i == 0) {
                auto notKey = packet; notKey.keyframe = false;
                RejectNewStream(notKey);
            }
        }
    }
    Require(rejectedPredictedFrame, "No predicted frame was available to verify fresh-stream rejection");
    std::cout << CodecName(codec) << ": immediate output, metadata/kind, BT.709 colors/range, reconnect, resize/odd height, bitrate reconfigure and non-keyframe rejection PASS\n";
}
}
int main(int argc, char** argv) {
    try {
        CheckInvalidContract();
        if (argc == 2 && std::string(argv[1]) == "--validation-only") return 0;
        const uint32_t encode = HardwareEncodeCapabilities(), decode = HardwareDecodeCapabilities(), both = encode & decode;
        std::cout << "Hardware capability masks: encode=" << encode << ", decode=" << decode << '\n';
        if (!both) { std::cout << "SKIP: no common NVIDIA hardware encoder/decoder\n"; return 77; }
        for (CodecId codec : {CodecId::H264, CodecId::Hevc, CodecId::Av1}) {
            if (both & CodecBit(codec)) CheckCodec(codec);
            else std::cout << CodecName(codec) << ": unsupported by local hardware pair; omitted\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
