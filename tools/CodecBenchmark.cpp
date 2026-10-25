#include "media/Codec.h"
#include <Windows.h>
#include <algorithm>
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
using lan3d::Frame;
using lan3d::media::EncodedFrame;
namespace {
constexpr unsigned Width = 3840, Height = 2160, BankSize = 12, Warmup = 12, Measured = 60;
double Milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c >= 32) out += char(c);
    }
    return out;
}
std::string CpuName() {
    char value[256]{}; DWORD bytes = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS) return value;
    return "Unavailable";
}
uint32_t Hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16);
}
// Precomputed, deterministic detailed scene: stereo disparity, textured walls,
// a moving checker panel, fine lines and gradients. Twelve frames consume 380 MiB.
// The cyclic bank deliberately changes at every input, including its wrap.
std::vector<Frame> MakeFrames() {
    std::vector<Frame> bank(BankSize);
    for (unsigned i = 0; i < BankSize; ++i) {
        auto& f = bank[i]; f.width = Width; f.height = Height; f.stride = Width * 4;
        f.kind = lan3d::ContentKind::LeiaSbs; f.bgra.resize(size_t(f.stride) * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const unsigned eye = x / (Width / 2);
            const unsigned sx = (x % (Width / 2)) + i * 3 + eye * 12;
            const unsigned sy = y + i * 2;
            const unsigned fine = Hash((sx / 3) * 719u + (sy / 3) * 104729u) & 31;
            const bool panel = ((sx / 160) ^ (sy / 160)) & 1;
            const bool line = sx % 71 < 2 || sy % 83 < 2;
            auto* p = f.bgra.data() + size_t(y) * f.stride + x * 4;
            p[0] = uint8_t(line ? 230 : 40 + ((sx / 19 + sy / 31) % 112) + fine);
            p[1] = uint8_t(line ? 225 : (panel ? 110 : 40) + ((sy / 17) % 62) + fine);
            p[2] = uint8_t(line ? 220 : (panel ? 45 : 110) + ((sx / 23) % 58) + fine);
            p[3] = 255;
        }
    }
    return bank;
}
struct Stats {
    double median{}, p95{}, mean{}, wallMs{}, throughput{};
};
Stats Summarize(std::vector<double> samples, double wallMs, size_t frames) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    return { (samples[(samples.size()-1)/2] + samples[samples.size()/2])/2,
        samples[size_t(std::ceil(samples.size() * .95)) - 1],
        std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size(),
        wallMs, frames * 1000.0 / wallMs };
}
void WriteStats(std::ostream& out, const Stats& s) {
    out << "{\"median_ms\":" << s.median << ",\"p95_ms\":" << s.p95
        << ",\"mean_ms\":" << s.mean << ",\"wall_ms\":" << s.wallMs
        << ",\"wall_fps\":" << s.throughput << "}";
}
struct Result {
    unsigned fps{}, bitrate{}; std::string error;
    Stats encode, decode, serial;
    size_t measuredPackets{}, measuredDecoded{}, serialDecoded{}, bytes{}, maxPacket{};
    uint64_t checksum{};
};
Result Run(std::vector<Frame>& bank, unsigned fps, unsigned bitrate, std::ofstream& csv) {
    Result r; r.fps = fps; r.bitrate = bitrate;
    std::string error;
    lan3d::media::Encoder encoder;
    if (!encoder.Configure(Width, Height, fps, bitrate, error)) { r.error = error; return r; }
    std::vector<EncodedFrame> encoded;
    std::vector<double> encodeTimes, decodeTimes, serialTimes;
    Clock::time_point encodeStart;
    for (unsigned n = 0; n < Warmup + Measured; ++n) {
        auto& f = bank[n % bank.size()]; f.sequence = n + 1; f.timestamp100ns = (n + 1) * (10000000ull / fps);
        if (n == Warmup) encodeStart = Clock::now();
        std::vector<EncodedFrame> packets;
        const auto start = Clock::now();
        if (!encoder.Encode(f, packets, error)) { r.error = error; return r; }
        const double elapsed = Milliseconds(start, Clock::now());
        if (n >= Warmup) {
            encodeTimes.push_back(elapsed);
            csv << fps << ',' << bitrate << ",encode," << n-Warmup << ',' << elapsed << ',' << packets.size() << '\n';
        }
        for (auto& packet : packets) {
            if (packet.sequence > Warmup) { ++r.measuredPackets; r.bytes += packet.data.size(); r.maxPacket = std::max(r.maxPacket, packet.data.size()); }
            encoded.push_back(std::move(packet));
        }
    }
    const double encodeWall = Milliseconds(encodeStart, Clock::now());
    r.encode = Summarize(encodeTimes, encodeWall, r.measuredPackets);
    lan3d::media::Decoder decoder;
    Clock::time_point decodeStart;
    bool decodeMeasuredStarted{};
    for (const auto& packet : encoded) {
        const bool measured = packet.sequence > Warmup;
        if (measured && !decodeMeasuredStarted) { decodeStart = Clock::now(); decodeMeasuredStarted = true; }
        std::vector<Frame> decoded;
        const auto start = Clock::now();
        if (!decoder.Decode(packet, decoded, error)) { r.error = error; return r; }
        const double elapsed = Milliseconds(start, Clock::now());
        if (measured) { decodeTimes.push_back(elapsed); csv << fps << ',' << bitrate << ",decode," << packet.sequence-Warmup-1 << ',' << elapsed << ',' << decoded.size() << '\n'; }
        for (const auto& frame : decoded) {
            if (frame.width != Width || frame.height != Height || frame.kind != lan3d::ContentKind::LeiaSbs) { r.error = "decoded dimensions or kind mismatch"; return r; }
            if (frame.sequence > Warmup) ++r.measuredDecoded;
            r.checksum += frame.bgra[frame.bgra.size()/3];
        }
    }
    r.decode = Summarize(decodeTimes, Milliseconds(decodeStart, Clock::now()), r.measuredDecoded);
    // An additional real serial encode/decode pass quantifies same-machine cost;
    // it is deliberately not labelled as a two-PC pipeline estimate.
    lan3d::media::Encoder serialEncoder;
    lan3d::media::Decoder serialDecoder;
    if (!serialEncoder.Configure(Width, Height, fps, bitrate, error)) { r.error = error; return r; }
    Clock::time_point serialStart;
    for (unsigned n = 0; n < Warmup + Measured; ++n) {
        auto& f = bank[n % bank.size()]; f.sequence = n + 1; f.timestamp100ns = (n + 1) * (10000000ull / fps);
        if (n == Warmup) serialStart = Clock::now();
        std::vector<EncodedFrame> packets;
        const auto start = Clock::now();
        if (!serialEncoder.Encode(f, packets, error)) { r.error = error; return r; }
        size_t count{};
        for (const auto& packet : packets) {
            std::vector<Frame> decoded;
            if (!serialDecoder.Decode(packet, decoded, error)) { r.error = error; return r; }
            for (const auto& frame : decoded) { if (frame.sequence > Warmup) { ++r.serialDecoded; ++count; } r.checksum += frame.bgra[frame.bgra.size()/3]; }
        }
        const double elapsed = Milliseconds(start, Clock::now());
        if (n >= Warmup) { serialTimes.push_back(elapsed); csv << fps << ',' << bitrate << ",serial," << n-Warmup << ',' << elapsed << ',' << count << '\n'; }
    }
    r.serial = Summarize(serialTimes, Milliseconds(serialStart, Clock::now()), r.serialDecoded);
    if (r.measuredPackets != Measured || r.measuredDecoded != Measured || r.serialDecoded != Measured)
        r.error = "frame counts differ; codec buffering must be considered before interpreting throughput";
    return r;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : L"results.json";
        std::filesystem::create_directories(output.parent_path());
        std::ofstream json(output), csv(output.parent_path() / "samples.csv");
        if (!json || !csv) throw std::runtime_error("Cannot create benchmark results");
        csv << "nominal_fps,target_bitrate_bps,stage,measured_index,milliseconds,output_frames\n";
        csv << std::fixed << std::setprecision(6); json << std::fixed << std::setprecision(6);
        std::cout << "Pre-generating twelve detailed moving stereo frames (380 MiB)...\n" << std::flush;
        auto bank = MakeFrames();
        json << "{\n\"width\":" << Width << ",\"height\":" << Height << ",\"eye_width\":" << Width/2
            << ",\"warmup_frames\":" << Warmup << ",\"measured_frames\":" << Measured
            << ",\"cpu\":\"" << Escape(CpuName()) << "\",\"gpu_used_for_codec\":false,\n"
            << "\"method\":\"Current production Codec.cpp, Release /O2, Windows software H264; pre-generated deterministic 12-frame moving detailed SBS bank. Encode includes BGRA-to-NV12; decode includes NV12-to-BGRA. One thread calls the codec. Generation excluded. No capture, GPU upload, network, render, or Leia interweaving. Serial is an additional same-PC encode-then-decode pass. FPS is throughput, not real-game acceptance.\",\n\"results\":[\n";
        bool first = true; int failures{};
        for (const auto [fps, bitrate] : std::vector<std::pair<unsigned,unsigned>>{{60,40000000},{60,80000000},{30,40000000},{30,80000000}}) {
            std::cout << "Running " << fps << " FPS nominal, " << bitrate/1000000 << " Mbps...\n" << std::flush;
            const auto r = Run(bank, fps, bitrate, csv);
            if (!first) json << ",\n"; first = false;
            json << "{\"nominal_fps\":" << fps << ",\"target_bitrate_bps\":" << bitrate << ",\"error\":\"" << Escape(r.error)
                << "\",\"measured_encoded_frames\":" << r.measuredPackets << ",\"measured_decoded_frames\":" << r.measuredDecoded
                << ",\"serial_decoded_frames\":" << r.serialDecoded << ",\"compressed_bytes\":" << r.bytes
                << ",\"mean_bytes_per_frame\":" << (r.measuredPackets ? double(r.bytes)/r.measuredPackets : 0)
                << ",\"largest_packet_bytes\":" << r.maxPacket
                << ",\"video_payload_mbps_at_nominal_fps\":" << (r.measuredPackets ? double(r.bytes)*8*fps/r.measuredPackets/1000000 : 0)
                << ",\"checksum\":" << r.checksum << ",\"encode\":"; WriteStats(json,r.encode);
            json << ",\"decode\":"; WriteStats(json,r.decode); json << ",\"serial\":"; WriteStats(json,r.serial); json << '}';
            json.flush(); csv.flush();
            if (!r.error.empty()) { ++failures; std::cout << "FAILED: " << r.error << '\n'; }
            else std::cout << "Encode median/p95 " << r.encode.median << '/' << r.encode.p95 << " ms; " << r.encode.throughput
                << " FPS. Decode " << r.decode.median << '/' << r.decode.p95 << " ms; " << r.decode.throughput
                << " FPS. Serial " << r.serial.throughput << " FPS. Payload " << double(r.bytes)*8*fps/r.measuredPackets/1000000 << " Mbps\n";
            std::cout << std::flush;
        }
        json << "\n]}\n"; return failures ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
