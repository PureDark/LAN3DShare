// Include the production sender so this diagnostic measures its actual Scale
// implementation. It never invokes RunSender, injects capture or starts a socket.
#include "../src/app/Sender.cpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>

namespace {
using BenchClock = std::chrono::steady_clock;
constexpr unsigned Warmup = 12, Measured = 120;
double Milliseconds(BenchClock::time_point from) {
    return std::chrono::duration<double, std::milli>(BenchClock::now() - from).count();
}
void WriteStats(std::ostream& out, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    out << "{\"mean_ms\":" << mean << ",\"p50_ms\":" << values[values.size() / 2]
        << ",\"p95_ms\":" << values[size_t(std::ceil(values.size() * .95)) - 1]
        << ",\"max_ms\":" << values.back() << ",\"stage_fps\":" << 1000.0 / mean << '}';
}
}

int wmain(int argc, wchar_t** argv) try {
    const std::filesystem::path output = argc > 1 ? argv[1] : L"build/benchmarks/sender-pipeline.json";
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream json(output);
    if (!json) throw std::runtime_error("Cannot create benchmark output");
    json << std::fixed << std::setprecision(6)
        << "{\n\"warmup_frames\":" << Warmup << ",\"measured_frames\":" << Measured
        << ",\"method\":\"Production Sender.cpp Scale called directly. Frame copy and CPU scaling are measured separately. Production hardware Encode includes BGRA upload, GPU conversion and bitstream retrieval. Repeated synthetic SBS frame; excludes game capture, IPC, network, receiver and interweaving. This is stage throughput, not end-to-end acceptance.\",\n\"results\":[\n";
    auto source = Pattern(7680, 2160, 1);
    lan3d::Frame scaled;
    uint64_t checksum{};
    std::vector<double> copies, scales;
    for (unsigned i = 0; i < Warmup + Measured; ++i) {
        auto start = BenchClock::now();
        auto copy = source;
        const auto copyMs = Milliseconds(start);
        start = BenchClock::now();
        scaled = Scale(std::move(copy), 3840, 2160);
        const auto scaleMs = Milliseconds(start);
        checksum += scaled.bgra[(size_t(i) * 104729) % scaled.bgra.size()];
        if (i >= Warmup) { copies.push_back(copyMs); scales.push_back(scaleMs); }
    }
    json << "{\"stage\":\"source_copy\",\"width\":7680,\"height\":2160,\"bytes\":" << source.bgra.size() << ",\"timing\":";
    WriteStats(json, copies); json << "},\n{\"stage\":\"cpu_scale\",\"input_width\":7680,\"input_height\":2160,\"width\":"
        << scaled.width << ",\"height\":" << scaled.height << ",\"timing\":";
    WriteStats(json, scales); json << '}'; json.flush();
    std::cout << "Production CPU Scale 7680x2160 -> " << scaled.width << 'x' << scaled.height << ": ";
    WriteStats(std::cout, scales); std::cout << '\n' << std::flush;
    const bool cpuOnly = argc > 2 && std::wstring(argv[2]) == L"--cpu-only";
    if (!cpuOnly) {
        const auto supported = lan3d::media::HardwareEncodeCapabilities();
        for (auto codec : {lan3d::media::CodecId::Av1, lan3d::media::CodecId::Hevc}) {
            if (!(supported & lan3d::media::CodecBit(codec))) continue;
            for (unsigned height : {1080u, 2160u}) {
                auto frame = height == 1080 ? scaled : Pattern(3840, height, 1);
                lan3d::media::Encoder encoder;
                std::string error;
                if (!encoder.Configure({3840, height, 60, 200000000, {}, codec}, true, error)) throw std::runtime_error(error);
                std::vector<double> times;
                uint64_t bytes{};
                for (unsigned i = 0; i < Warmup + Measured; ++i) {
                    frame.sequence = i + 1; frame.timestamp100ns = uint64_t(i + 1) * 10000000 / 60;
                    std::vector<lan3d::media::EncodedFrame> packets;
                    const auto start = BenchClock::now();
                    if (!encoder.Encode(frame, packets, error)) throw std::runtime_error(error);
                    const auto elapsed = Milliseconds(start);
                    if (!encoder.hardware() || packets.size() != 1 || packets.front().sequence != frame.sequence)
                        throw std::runtime_error("Missing immediate matching hardware packet");
                    if (i >= Warmup) { times.push_back(elapsed); bytes += packets.front().data.size(); }
                }
                json << ",\n{\"stage\":\"cpu_input_nvenc\",\"codec\":\"" << lan3d::media::CodecName(codec)
                    << "\",\"width\":3840,\"height\":" << height << ",\"bitrate_bps\":200000000,\"compressed_bytes\":"
                    << bytes << ",\"timing\":";
                WriteStats(json, times); json << '}'; json.flush();
                std::cout << "Production " << lan3d::media::CodecName(codec) << " 3840x" << height << ": ";
                WriteStats(std::cout, times); std::cout << '\n' << std::flush;
            }
        }
    }
    json << "\n],\"checksum\":" << checksum << "}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "SenderPipelineBenchmark: " << error.what() << '\n';
    return 1;
}
