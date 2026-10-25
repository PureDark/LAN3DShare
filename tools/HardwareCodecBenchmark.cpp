#include "media/Codec.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace lan3d;
using namespace lan3d::media;
namespace {
constexpr unsigned Width = 3840, Height = 2160, Fps = 60;
constexpr std::array<unsigned, 3> Bitrates{100000000, 200000000, 400000000};
constexpr unsigned BankSize = 12, Warmup = 12, Measured = 120;
constexpr unsigned QualityFrames = 12, EdgeThreshold = 32;
double Milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
std::string Escape(const std::string& value) {
    std::string result;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c == '\n') result += "\\n";
        else if (c >= 32) result += char(c);
    }
    return result;
}
uint32_t Hash(uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; return value ^ (value >> 16);
}
// Same deterministic scene family as the software benchmark: stereo disparity,
// moving textured panels, fine lines and gradients. Generation is not timed.
std::vector<Frame> MakeFrames() {
    std::vector<Frame> bank(BankSize);
    for (unsigned i = 0; i < BankSize; ++i) {
        auto& frame = bank[i]; frame.width = Width; frame.height = Height; frame.stride = Width * 4;
        frame.kind = ContentKind::LeiaSbs; frame.bgra.resize(size_t(frame.stride) * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const unsigned eye = x / (Width / 2), sx = (x % (Width / 2)) + i * 3 + eye * 12, sy = y + i * 2;
            const unsigned fine = Hash((sx / 3) * 719u + (sy / 3) * 104729u) & 31;
            const bool panel = ((sx / 160) ^ (sy / 160)) & 1;
            const bool line = sx % 71 < 2 || sy % 83 < 2;
            auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + x * 4;
            pixel[0] = uint8_t(line ? 230 : 40 + ((sx / 19 + sy / 31) % 112) + fine);
            pixel[1] = uint8_t(line ? 225 : (panel ? 110 : 40) + ((sy / 17) % 62) + fine);
            pixel[2] = uint8_t(line ? 220 : (panel ? 45 : 110) + ((sx / 23) % 58) + fine);
            pixel[3] = 255;
        }
    }
    return bank;
}
struct Stats { double p50{}, p95{}, mean{}, wallMs{}, fps{}; };
Stats Summarize(std::vector<double> samples, double wallMs, size_t frames) {
    if (samples.empty() || wallMs <= 0) return {};
    std::sort(samples.begin(), samples.end());
    return {(samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2,
        samples[size_t(std::ceil(samples.size() * .95)) - 1],
        std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size(), wallMs, frames * 1000.0 / wallMs};
}
void WriteStats(std::ostream& output, const Stats& stats) {
    output << "{\"p50_ms\":" << stats.p50 << ",\"p95_ms\":" << stats.p95 << ",\"mean_ms\":" << stats.mean
        << ",\"wall_ms\":" << stats.wallMs << ",\"wall_fps\":" << stats.fps << '}';
}
struct EyeQuality {
    uint64_t squaredError{}, absoluteError{}, components{}, edgeError{}, edgeComponents{};
    double Mse() const { return components ? double(squaredError) / components : 0; }
    double Mae() const { return components ? double(absoluteError) / components : 0; }
    double EdgeMae() const { return edgeComponents ? double(edgeError) / edgeComponents : 0; }
};
struct Quality {
    unsigned frames{};
    std::array<EyeQuality, 2> eyes;
};
void AddEdge(EyeQuality& quality, const uint8_t* source, const uint8_t* sourceNext,
    const uint8_t* decoded, const uint8_t* decodedNext) {
    int sourceGradient[3]{}, decodedGradient[3]{}, largest{};
    for (unsigned c = 0; c < 3; ++c) {
        sourceGradient[c] = int(sourceNext[c]) - source[c];
        decodedGradient[c] = int(decodedNext[c]) - decoded[c];
        largest = std::max(largest, std::abs(sourceGradient[c]));
    }
    if (largest < int(EdgeThreshold)) return;
    for (unsigned c = 0; c < 3; ++c)
        quality.edgeError += unsigned(std::abs(sourceGradient[c] - decodedGradient[c]));
    quality.edgeComponents += 3;
}
void Compare(Quality& quality, const Frame& source, const Frame& decoded) {
    const unsigned eyeWidth = source.width / 2;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& stats = quality.eyes[eye];
        for (unsigned y = 0; y < source.height; ++y) for (unsigned x = 0; x < eyeWidth; ++x) {
            const auto* a = source.bgra.data() + size_t(y) * source.stride + (eye * eyeWidth + x) * 4;
            const auto* b = decoded.bgra.data() + size_t(y) * decoded.stride + (eye * eyeWidth + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const int delta = int(a[c]) - b[c];
                stats.squaredError += unsigned(delta * delta);
                stats.absoluteError += unsigned(std::abs(delta));
            }
            stats.components += 3;
            // Exclude the SBS eye seam. Compare signed first differences only
            // where the source has a strong horizontal or vertical RGB edge.
            if (x + 1 < eyeWidth) AddEdge(stats, a, a + 4, b, b + 4);
            if (y + 1 < source.height) AddEdge(stats, a, a + source.stride, b, b + decoded.stride);
        }
    }
    ++quality.frames;
}
void WriteQuality(std::ostream& output, const Quality& quality) {
    output << "{\"sampled_frames\":" << quality.frames << ",\"edge_threshold_code_values\":" << EdgeThreshold << ",\"eyes\":[";
    for (unsigned eye = 0; eye < quality.eyes.size(); ++eye) {
        const auto& stats = quality.eyes[eye];
        if (eye) output << ',';
        output << "{\"eye\":\"" << (eye ? "right" : "left") << "\",\"rgb_psnr_db\":";
        if (stats.squaredError && stats.components) output << 10 * std::log10(255.0 * 255.0 / stats.Mse());
        else output << "null"; // Exact equality has infinite PSNR, which JSON cannot represent.
        output << ",\"rgb_exact\":" << (stats.components && !stats.squaredError ? "true" : "false")
            << ",\"rgb_mae_code_values\":" << stats.Mae() << ",\"rgb_mse\":" << stats.Mse()
            << ",\"rgb_component_count\":" << stats.components
            << ",\"edge_gradient_mae_code_values\":" << stats.EdgeMae()
            << ",\"edge_component_count\":" << stats.edgeComponents << '}';
    }
    output << "]}";
}
struct Result {
    std::string error;
    Stats encode, decode;
    bool hardwareEncode{}, hardwareDecode{};
    size_t encodedFrames{}, decodedFrames{}, bytes{}, maxPacket{};
    uint64_t checksum{};
    Quality quality;
};
Result Run(std::vector<Frame>& bank, CodecId codec, unsigned targetBitrate, std::ofstream& samples) {
    Result result;
    Encoder encoder;
    std::string error;
    if (!encoder.Configure(CodecConfig{Width, Height, Fps, targetBitrate, {}, codec}, true, error)) { result.error = "Configure encoder: " + error; return result; }
    result.hardwareEncode = encoder.hardware();
    if (!result.hardwareEncode) { result.error = "Hardware encoder fell back to software"; return result; }
    std::vector<EncodedFrame> encoded;
    encoded.reserve(Warmup + Measured);
    std::vector<double> encodeTimes, decodeTimes;
    encodeTimes.reserve(Measured); decodeTimes.reserve(Measured);
    Clock::time_point encodeStart;
    for (unsigned n = 0; n < Warmup + Measured; ++n) {
        auto& frame = bank[n % bank.size()]; frame.sequence = n + 1; frame.timestamp100ns = uint64_t(n + 1) * 10000000 / Fps;
        if (n == Warmup) encodeStart = Clock::now();
        std::vector<EncodedFrame> packets;
        const auto start = Clock::now();
        if (!encoder.Encode(frame, packets, error)) { result.error = "Encode: " + error; return result; }
        const double elapsed = Milliseconds(start, Clock::now());
        if (packets.size() != 1 || packets.front().sequence != frame.sequence || packets.front().config.codec != codec) {
            result.error = "Each encode input must immediately produce one matching packet"; return result;
        }
        if (n >= Warmup) {
            encodeTimes.push_back(elapsed); ++result.encodedFrames;
            result.bytes += packets.front().data.size(); result.maxPacket = std::max(result.maxPacket, packets.front().data.size());
            samples << CodecName(codec) << ',' << targetBitrate << ",encode," << n - Warmup << ',' << elapsed << ",1\n";
        }
        encoded.push_back(std::move(packets.front()));
    }
    result.encode = Summarize(encodeTimes, Milliseconds(encodeStart, Clock::now()), result.encodedFrames);
    {
    Decoder decoder(true);
    Clock::time_point decodeStart;
    for (unsigned n = 0; n < encoded.size(); ++n) {
        if (n == Warmup) decodeStart = Clock::now();
        const auto& packet = encoded[n];
        std::vector<Frame> frames;
        const auto start = Clock::now();
        if (!decoder.Decode(packet, frames, error)) { result.error = "Decode: " + error; return result; }
        const double elapsed = Milliseconds(start, Clock::now());
        result.hardwareDecode = decoder.hardware();
        if (!result.hardwareDecode) { result.error = "Hardware decoder fell back to software"; return result; }
        if (frames.size() != 1 || frames.front().sequence != packet.sequence || frames.front().timestamp100ns != packet.timestamp100ns ||
            frames.front().width != Width || frames.front().height != Height || frames.front().kind != ContentKind::LeiaSbs || !ValidFrame(frames.front())) {
            result.error = "Each decode input must immediately produce one matching valid frame"; return result;
        }
        if (n >= Warmup) {
            decodeTimes.push_back(elapsed); ++result.decodedFrames;
            samples << CodecName(codec) << ',' << targetBitrate << ",decode," << n - Warmup << ',' << elapsed << ",1\n";
        }
        result.checksum += frames.front().bgra[frames.front().bgra.size() / 3];
    }
    result.decode = Summarize(decodeTimes, Milliseconds(decodeStart, Clock::now()), result.decodedFrames);
    }
    if (result.encodedFrames != Measured || result.decodedFrames != Measured) { result.error = "Measured frame count mismatch"; return result; }

    // Independent untimed pass: replay from the initial keyframe so P-frame
    // references are identical. Keep only the current decoded BGRA frame.
    std::array<bool, Warmup + Measured> selected{};
    for (unsigned i = 0; i < QualityFrames; ++i) selected[Warmup + (i * 13) % Measured] = true;
    Decoder qualityDecoder(true);
    for (unsigned n = 0; n < encoded.size(); ++n) {
        std::vector<Frame> frames;
        if (!qualityDecoder.Decode(encoded[n], frames, error)) { result.error = "Quality decode: " + error; return result; }
        if (!qualityDecoder.hardware() || frames.size() != 1 || !ValidFrame(frames.front()) ||
            frames.front().width != Width || frames.front().height != Height ||
            frames.front().sequence != encoded[n].sequence || frames.front().timestamp100ns != encoded[n].timestamp100ns ||
            frames.front().kind != ContentKind::LeiaSbs) {
            result.error = "Quality replay did not produce the matching hardware-decoded frame"; return result;
        }
        if (selected[n]) Compare(result.quality, bank[n % bank.size()], frames.front());
    }
    if (result.quality.frames != QualityFrames) result.error = "Quality sample count mismatch";
    return result;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        const std::filesystem::path outputPath = argc > 1 ? argv[1] : L"build/benchmarks/hardware-results.json";
        if (!outputPath.parent_path().empty()) std::filesystem::create_directories(outputPath.parent_path());
        std::ofstream json(outputPath), samples(outputPath.parent_path() / "hardware-samples.csv");
        if (!json || !samples) throw std::runtime_error("Cannot create hardware benchmark results");
        json << std::fixed << std::setprecision(6); samples << std::fixed << std::setprecision(6);
        samples << "codec,target_bitrate_bps,stage,measured_index,milliseconds,output_frames\n";
        const uint32_t encode = HardwareEncodeCapabilities(), decode = HardwareDecodeCapabilities(), both = encode & decode;
        json << "{\n\"width\":" << Width << ",\"height\":" << Height << ",\"eye_width\":" << Width / 2
            << ",\"nominal_fps\":" << Fps << ",\"target_bitrates_bps\":[100000000,200000000,400000000]"
            << ",\"warmup_frames\":" << Warmup << ",\"measured_frames\":" << Measured
            << ",\"encode_capability_mask\":" << encode << ",\"decode_capability_mask\":" << decode
            << ",\n\"method\":\"Production hardware codec, same pre-generated deterministic 12-frame detailed moving SBS bank for all codecs and bitrates. Encode includes CPU BGRA upload, GPU color conversion and NVENC bitstream retrieval. Decode includes NVDEC, GPU color conversion and BGRA readback to CPU. Stages measured separately on this PC with 12 warmup and 120 timed frames; wall throughput includes the timed loop and its packet checks and CSV recording, but excludes quality analysis. Excludes generation, capture, network, rendering and Leia interweaving. Throughput is not end-to-end or real-game acceptance.\","
            << "\n\"quality_method\":\"Separate untimed hardware-decoder replay from the first keyframe; 12 samples selected at measured indices (i*13)%120 for i=0..11, covering all 12 source phases. Per-eye RGB PSNR uses pooled MSE over all sampled 8-bit sRGB B/G/R components, excluding alpha. RGB MAE uses the same components. Edge error is MAE of signed horizontal/vertical RGB first-difference reconstruction where any source channel difference is at least 32; the SBS seam is excluded. Conversion and 4:2:0 chroma loss are included. Infinite PSNR is JSON null with rgb_exact=true. PSNR and synthetic-scene metrics do not establish perceptually lossless quality.\",\n\"results\":[\n";
        std::vector<Frame> bank;
        if (both & (CodecBit(CodecId::Hevc) | CodecBit(CodecId::Av1))) {
            std::cout << "Pre-generating twelve detailed moving stereo frames (380 MiB)...\n" << std::flush;
            bank = MakeFrames();
        }
        unsigned failures{}, runs{}; bool first = true;
        for (CodecId codec : {CodecId::Hevc, CodecId::Av1}) for (unsigned targetBitrate : Bitrates) {
            if (!first) json << ",\n"; first = false;
            json << "{\"codec\":\"" << CodecName(codec) << "\",\"target_bitrate_bps\":" << targetBitrate << ",\"supported\":";
            if (!(both & CodecBit(codec))) { json << "false,\"error\":\"No common local hardware support\"}"; continue; }
            ++runs;
            std::cout << "Running " << CodecName(codec) << " at 3840x2160, target 60 FPS / " << targetBitrate / 1000000 << " Mbps...\n" << std::flush;
            const auto result = Run(bank, codec, targetBitrate, samples);
            const double bitrate = result.encodedFrames ? double(result.bytes) * 8 * Fps / result.encodedFrames / 1000000 : 0;
            json << "true,\"error\":\"" << Escape(result.error) << "\",\"hardware_encode\":" << (result.hardwareEncode ? "true" : "false")
                << ",\"hardware_decode\":" << (result.hardwareDecode ? "true" : "false")
                << ",\"measured_encoded_frames\":" << result.encodedFrames << ",\"measured_decoded_frames\":" << result.decodedFrames
                << ",\"compressed_bytes\":" << result.bytes << ",\"largest_packet_bytes\":" << result.maxPacket
                << ",\"video_payload_mbps_at_nominal_fps\":" << bitrate << ",\"checksum\":" << result.checksum << ",\"encode\":";
            WriteStats(json, result.encode); json << ",\"decode\":"; WriteStats(json, result.decode);
            json << ",\"quality\":"; WriteQuality(json, result.quality); json << '}';
            json.flush(); samples.flush();
            if (!result.error.empty()) { ++failures; std::cout << "FAILED: " << result.error << '\n'; }
            else std::cout << "Encode p50/p95 " << result.encode.p50 << '/' << result.encode.p95 << " ms, " << result.encode.fps
                << " FPS; decode " << result.decode.p50 << '/' << result.decode.p95 << " ms, " << result.decode.fps
                << " FPS; payload " << bitrate << " Mbps; left/right RGB MAE "
                << result.quality.eyes[0].Mae() << '/' << result.quality.eyes[1].Mae() << '\n';
            std::cout << std::flush;
        }
        json << "\n]}\n";
        return failures ? 1 : runs ? 0 : 77;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
