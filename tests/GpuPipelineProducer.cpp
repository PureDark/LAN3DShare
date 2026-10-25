#include "common/FrameBus.h"
#include "common/GpuFrameBus.h"
#include "net/Transport.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
using namespace lan3d;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace {
void Require(bool okay, const std::string& message) { if (!okay) throw std::runtime_error(message); }
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { char text[160]{}; sprintf_s(text, "%s: 0x%08X", operation, unsigned(hr)); throw std::runtime_error(text); }
}
struct SharedScene {
    static constexpr uint32_t Width = 7680, Height = 2160;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>, 3> textures;
    std::array<HANDLE, 3> shared{};
    std::array<GpuFrame, 3> frames;
    ~SharedScene() { for (auto handle : shared) if (handle) CloseHandle(handle); }
    bool Initialize() {
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        LUID luid{};
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(n, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10DE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                luid = desc.AdapterLuid; break;
            }
        }
        if (!device) return false;
        D3D12_COMMAND_QUEUE_DESC q{};
        Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "test producer queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "test allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "test commands");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "test fence");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 3;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs)), "test RTV heap");
        const UINT increment = device->GetDescriptorHandleIncrementSize(heap.Type);
        for (unsigned n = 0; n < textures.size(); ++n) {
            auto& metadata = frames[n];
            metadata.width = Width; metadata.height = Height;
            metadata.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            metadata.adapterLuid = luid; metadata.sequence = n + 1; metadata.timestamp100ns = n + 1;
            swprintf_s(metadata.textureName, L"Local\\LAN3DShare.GpuPipeline.%lu.%llu.%u",
                GetCurrentProcessId(), GetTickCount64(), n);
            D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC texture{};
            texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = Width; texture.Height = Height;
            texture.DepthOrArraySize = 1; texture.MipLevels = 1; texture.Format = metadata.format;
            texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &texture,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&textures[n])), "shared D3D12 input");
            Check(device->CreateSharedHandle(textures[n].Get(), nullptr, GENERIC_ALL, metadata.textureName, &shared[n]), "named shared texture");
            auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += n * increment;
            device->CreateRenderTargetView(textures[n].Get(), nullptr, rtv);
            // Distinct eye regions and patterns remain immutable after this
            // initialization. The 60 Hz producer never maps or copies pixels.
            for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 32; ++x) {
                const unsigned eye = x / 16;
                const float shade = float(16 + ((x * 37 + y * 23 + n * 49) % 192)) / 255.f;
                const float color[]{eye ? 0.08f : shade, float(40 + n * 45) / 255.f, eye ? shade : 0.08f, 1.f};
                const D3D12_RECT rect{LONG(x * (Width / 32)), LONG(y * (Height / 12)),
                    LONG((x + 1) * (Width / 32)), LONG((y + 1) * (Height / 12))};
                commands->ClearRenderTargetView(rtv, color, 1, &rect);
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = textures[n].Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commands->ResourceBarrier(1, &barrier);
        }
        Check(commands->Close(), "close producer commands");
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), 1), "producer completion signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event != nullptr, "producer completion event");
        Check(fence->SetEventOnCompletion(1, event), "producer completion event setup");
        const auto waited = WaitForSingleObject(event, 10000); CloseHandle(event);
        Require(waited == WAIT_OBJECT_0, "producer GPU completion timed out");
        // Match the production contract: producer completed, recording can no
        // longer execute, and resources stay immutable while consumers hold them.
        Check(allocator->Reset(), "producer allocator reset");
        Check(commands->Reset(allocator.Get(), nullptr), "producer list reset");
        Check(commands->Close(), "close empty producer list");
        return true;
    }
};
struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};
int VerifyModes(uint16_t port) {
    net::TcpConnection connection;
    media::Decoder decoder(true);
    std::string error;
    const auto capabilities = media::HardwareDecodeCapabilities();
    if (!capabilities) return 77;
    Require(connection.Connect("127.0.0.1", port, 5000, error) == net::IoResult::Ok, error);
    Require(connection.SendCapabilities(capabilities, 5000, error) == net::IoResult::Ok, error);
    uint64_t previous{};
    for (bool stereo : {true, false, true, false, true}) {
        Require(connection.SendStereoMode(stereo, 1000, error) == net::IoResult::Ok, error);
        const auto kind = stereo ? ContentKind::LeiaSbs : ContentKind::LeiaLeft;
        unsigned matching{};
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (matching < 4 && Clock::now() < deadline) {
            media::EncodedFrame packet;
            Require(connection.Receive(packet, 1500, error) == net::IoResult::Ok, error);
            Require(packet.sequence > previous, "Mode switch reset source sequence"); previous = packet.sequence;
            std::vector<Frame> frames;
            Require(decoder.Decode(packet, frames, error) && decoder.hardware() && frames.size() == 1, "Mode switch hardware decode: " + error);
            if (packet.kind != kind) continue; // In-flight old frames remain well tagged.
            Require(matching || packet.keyframe, "Mode change did not start with a keyframe");
            Require(packet.config.width == 3840 && packet.config.height == 2160 && packet.config.bitrate == 200000000 &&
                packet.displayWidth == 3840 && packet.displayHeight == 2160 && frames[0].kind == kind,
                "Sender mode change lost native eye resolution, bitrate or metadata");
            ++matching;
        }
        Require(matching == 4, "Sender did not react to receiver mode");
    }
    std::cout << "GPU_MODE_VERIFIED transitions=5 encoded=3840x2160 bitrate=200Mbps reconnects=0 hardware_decode=1\n" << std::flush;
    return 0;
}
int VerifyWire(uint16_t port, bool sink, unsigned measureSeconds) {
    net::TcpConnection connection;
    std::string error;
    // The sink accepts all opaque codec payloads and never initializes NVDEC.
    const auto capabilities = sink ? 7u : media::HardwareDecodeCapabilities();
    if (!capabilities) return 77;
    Require(connection.Connect("127.0.0.1", port, 5000, error) == net::IoResult::Ok, "connect verifier: " + error);
    Require(connection.SendCapabilities(capabilities, 5000, error) == net::IoResult::Ok, "verifier capabilities: " + error);
    std::unique_ptr<media::Decoder> decoder;
    if (!sink) decoder = std::make_unique<media::Decoder>(true);
    uint64_t previous{}, previousTime{};
    uint64_t measured{}, skipped{}, bytes{}, windowFrames{}, audioFrames{}, audioPackets{};
    std::vector<double> intervals, ages;
    Clock::time_point first{}, measuredStart{}, last{}, window{};
    for (unsigned n = 0;; ++n) {
        media::EncodedFrame packet;
        Require(connection.Receive(packet, 5000, error) == net::IoResult::Ok, "wire receive: " + error);
        if (!packet.audio.samples.empty()) { audioFrames += packet.audio.Frames(); ++audioPackets; }
        Require(packet.config.width == 3840 && packet.config.height == 2160 && packet.config.fps == 60 &&
            packet.displayWidth == 3840 && packet.displayHeight == 2160 && packet.kind == ContentKind::LeiaSbs,
            "Wire v5 lost total SBS dimensions, original eye aspect or content kind");
        Require((n || packet.keyframe) && packet.sequence > previous && packet.timestamp100ns > previousTime,
            "Reconnect did not begin at a keyframe or source metadata failed to advance");
        if (decoder) {
            std::vector<Frame> output;
            Require(decoder->Decode(packet, output, error) && decoder->hardware() && output.size() == 1,
                "wire verifier NVDEC: " + error);
            Require(output[0].displayWidth == 3840 && output[0].displayHeight == 2160 &&
                output[0].width == 3840 && output[0].height == 2160, "Decoded frame lost display aspect");
        } else {
            const auto now = Clock::now();
            if (!n) first = now;
            if (measuredStart == Clock::time_point{} && now - first >= std::chrono::seconds(2))
                measuredStart = last = window = now;
            else if (measuredStart != Clock::time_point{}) {
                ++measured; ++windowFrames; skipped += packet.sequence - previous - 1;
                bytes += packet.data.size();
                intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());
                const auto time = Timestamp100ns();
                if (time >= packet.timestamp100ns) ages.push_back(double(time - packet.timestamp100ns) / 10000.);
                last = now;
                const double seconds = std::chrono::duration<double>(now - window).count();
                if (seconds >= 1.) {
                    std::cout << "WIRE_SINK_SAMPLE seconds=" << seconds << " frames=" << windowFrames
                        << " fps=" << windowFrames / seconds << '\n' << std::flush;
                    window = now; windowFrames = 0;
                }
            }
        }
        previous = packet.sequence; previousTime = packet.timestamp100ns;
        if (!sink && n == 3) break;
        if (sink && measuredStart != Clock::time_point{} && last - measuredStart >= std::chrono::seconds(measureSeconds)) break;
    }
    if (sink) {
        Require(measured >= 30ull * measureSeconds, "Wire sink received fewer than 30 FPS during its steady interval");
        const double seconds = std::chrono::duration<double>(last - measuredStart).count();
        std::sort(intervals.begin(), intervals.end());
        std::cout << "GPU_WIRE_SINK version=6 encoded=3840x2160 display_eye=3840x2160 decoded=0 rendered=0 warmup_seconds=2"
            << " measured_frames=" << measured << " measured_seconds=" << seconds << " fps=" << measured / seconds
            << " missing_source_sequences=" << skipped << " packet_interval_p50_ms=" << intervals[intervals.size() / 2]
            << " packet_interval_p95_ms=" << intervals[size_t(intervals.size() * .95)]
            << " frame_age_mean_ms=" << (ages.empty() ? 0. : std::accumulate(ages.begin(), ages.end(), 0.) / ages.size())
            << " payload_mbps=" << double(bytes) * 8. / seconds / 1000000. << " last_sequence=" << previous
            << " audio_sample_frames=" << audioFrames << " audio_packets=" << audioPackets << '\n' << std::flush;
        return 0;
    }
    std::cout << "GPU_WIRE_VERIFIED version=6 encoded=3840x2160 display_eye=3840x2160 keyframe=1 decoded=4 last_sequence="
        << previous << '\n' << std::flush;
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::wstring stopName;
        uint16_t verifyPort{};
        unsigned measureSeconds{10};
        bool sink{}, modes{};
        for (int n = 1; n < argc; ++n) {
            const std::wstring option = argv[n];
            if (option == L"--wire-sink") { sink = true; continue; }
            if (option == L"--verify-modes") { modes = true; continue; }
            Require(n + 1 < argc, "Missing producer argument");
            if (option == L"--stop-event") stopName = argv[++n];
            else if (option == L"--verify-port") {
                const auto port = std::stoul(argv[++n]);
                Require(port && port <= 65535, "Invalid verifier port"); verifyPort = uint16_t(port);
            } else if (option == L"--measure-seconds") {
                const auto seconds = std::stoul(argv[++n]);
                Require(seconds >= 1 && seconds <= 120, "Invalid measurement duration"); measureSeconds = unsigned(seconds);
            } else throw std::runtime_error("Unknown producer argument");
        }
        if (verifyPort) return modes ? VerifyModes(verifyPort) : VerifyWire(verifyPort, sink, measureSeconds);
        Require(!sink, "--wire-sink requires --verify-port");
        Require(!stopName.empty(), "--stop-event is required");
        Handle stop{OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str())};
        Require(stop.value != nullptr, "Cannot open producer stop event");
        SharedScene scene;
        if (!scene.Initialize()) { std::cout << "SKIP NVIDIA D3D12 unavailable\n" << std::flush; return 77; }
        FrameBus status; GpuFrameBus bus; std::string error;
        Require(status.Open(GetCurrentProcessId(), true, error) && bus.Open(GetCurrentProcessId(), true, error), error);
        status.SetStatus("Synthetic GPU producer; D3D12 shared textures; no CPU pixels");
        Handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        if (!timer.value) timer.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        Require(timer.value != nullptr, "Cannot create producer pacing timer");
        const std::array<HANDLE, 2> waits{stop.value, timer.value};
        std::array<uint64_t, 3> leases{};
        uint64_t sequence{}, dropped{}, tick{};
        const auto began = Clock::now();
        std::cout << "GPU_PRODUCER_READY pid=" << GetCurrentProcessId() << " source=7680x2160 target=60fps synthetic=1\n" << std::flush;
        while (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0 && Clock::now() - began < std::chrono::seconds(60)) {
            const auto target = began + std::chrono::nanoseconds(++tick * 1000000000ull / 60);
            bool published{};
            for (unsigned attempt = 0; attempt < leases.size(); ++attempt) {
                const unsigned index = unsigned((sequence + attempt) % leases.size());
                if (leases[index] && !bus.IsReleased(leases[index])) continue;
                auto frame = scene.frames[index]; frame.sequence = sequence + 1; frame.timestamp100ns = Timestamp100ns();
                if (bus.Publish(frame)) { leases[index] = ++sequence; published = true; }
                break;
            }
            if (!published) ++dropped;
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(target - Clock::now()).count();
            if (remaining > 0) {
                LARGE_INTEGER due{}; due.QuadPart = -std::max(1ll, remaining / 100);
                Require(SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE) != FALSE, "Producer timer failed");
                WaitForMultipleObjects(DWORD(waits.size()), waits.data(), FALSE, INFINITE);
            }
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
        std::cout << "GPU_PRODUCER_STOPPED published=" << sequence << " dropped=" << dropped << " seconds=" << seconds
            << " fps=" << sequence / seconds << " cpu_pixel_copies=0\n" << std::flush;
        return 0;
    } catch (const std::exception& error) { std::cerr << "GPU producer FAILED: " << error.what() << '\n'; return 1; }
}
