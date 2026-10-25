#include "media/Codec.h"
#include "media/GpuAdapter.h"
#include "common/GpuImage.h"
#include <d3d11_1.h>
#include "common/GpuFrameBus.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace lan3d;
using namespace lan3d::media;
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
            metadata.format = n < 2 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            metadata.adapterLuid = luid; metadata.sequence = n + 1; metadata.timestamp100ns = n + 1;
            swprintf_s(metadata.textureName, L"Local\\LAN3DShare.GpuEncodeTest.%lu.%llu.%u",
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
            // Two eye regions, many changing tile boundaries and distinct RGBA
            // versus BGRA resources exercise format, eye order and resizing.
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
void CheckPixels(const Frame& frame, unsigned pattern) {
    Require(ValidFrame(frame), "Decoded GPU input is invalid");
    for (unsigned y : {1u, 5u, 10u}) for (unsigned x : {1u, 7u, 17u, 25u}) {
        const unsigned px = (2 * x + 1) * frame.width / 64, py = (2 * y + 1) * frame.height / 24;
        const auto* pixel = frame.bgra.data() + size_t(py) * frame.stride + px * 4;
        const unsigned shade = 16 + ((x * 37 + y * 23 + pattern * 49) % 192);
        const std::array<unsigned, 3> expected{x >= 16 ? shade : 20u, 40u + pattern * 45, x >= 16 ? 20u : shade};
        for (unsigned channel = 0; channel < 3; ++channel)
            Require(std::abs(int(pixel[channel]) - int(expected[channel])) <= 8,
                "Shared GPU input eye order/format/scaling/color mismatch");
    }
}
// Diagnostic readback only: production decoder/rendering never downloads a full frame.
Frame ReadGpu(const Frame& input) {
    Require(input.bgra.empty() && input.gpu && ValidVideoFrame(input), "Decoder did not return a GPU-only frame");
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "readback factory");
    Check(factory->EnumAdapterByLuid(input.gpu->metadata.adapterLuid, IID_PPV_ARGS(&adapter)), "readback adapter");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "readback device");
    ComPtr<ID3D11Device1> device1; Check(device.As(&device1), "readback shared device");
    ComPtr<ID3D11Texture2D> texture, staging;
    Check(device1->OpenSharedResourceByName(input.gpu->metadata.textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&texture)), "readback shared image");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "diagnostic staging"); context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "diagnostic map");
    Frame output = input; output.gpu.reset(); output.stride = input.width * 4; output.bgra.resize(size_t(output.stride) * output.height);
    for (unsigned y = 0; y < output.height; ++y) for (unsigned x = 0; x < output.width; ++x) {
        const auto* from = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + x * 4;
        auto* to = output.bgra.data() + size_t(y) * output.stride + x * 4;
        to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = from[3];
    }
    context->Unmap(staging.Get(), 0); return output;
}
void Run(SharedScene& scene, CodecId codec, const LUID* target = nullptr) {
    std::string error;
    GpuFrameBus producer, consumer;
    Require(producer.Open(GetCurrentProcessId(), true, error), "GPU lease producer: " + error);
    Require(consumer.Open(GetCurrentProcessId(), false, error), "GPU lease consumer: " + error);
    Encoder encoder;
    const CodecConfig config{3840, 2160, 60, 200000000, {}, codec};
    const bool configured = encoder.ConfigureGpu(config, scene.frames[0], error, target);
    Require(configured, "ConfigureGpu: " + error);
    auto bad = scene.frames[0]; bad.adapterLuid.LowPart ^= 1;
    std::vector<EncodedFrame> packets;
    Require(!encoder.EncodeGpu(bad, packets, error) && packets.empty(), "Wrong adapter accepted");
    bad = scene.frames[0]; bad.width /= 2;
    Require(!encoder.EncodeGpu(bad, packets, error) && packets.empty(), "Wrong shared texture metadata accepted");
    constexpr unsigned Warmup = 12, Measured = 120;
    std::vector<double> durations;
    EncodeTiming timings;
    std::array<EncodedFrame, 2> samples;
    auto started = Clock::now();
    for (unsigned n = 0; n < Warmup + Measured; ++n) {
        auto input = scene.frames[n % 2];
        input.sequence = uint64_t(n) + 0x100000001ull;
        input.timestamp100ns = 0x200000000ull + uint64_t(n) * 10000000 / 60;
        Require(producer.Publish(input), "Publish immutable GPU frame");
        GpuFrame leased;
        Require(consumer.AcquireLatest(leased, input.sequence - 1) && leased.sequence == input.sequence,
            "Acquire GPU source lease");
        Require(!producer.IsReleased(input.sequence), "Producer reused a held GPU source");
        if (n == 1 || n == Warmup) Require(encoder.RequestKeyFrame(error), "RequestKeyFrame: " + error);
        if (n == Warmup) started = Clock::now();
        const auto before = Clock::now();
        const bool encoded = encoder.EncodeGpu(leased, packets, error);
        const auto after = Clock::now();
        Require(encoded, "EncodeGpu: " + error);
        // EncodeGpu only permits acknowledgement after its synchronous output
        // completed all VP and NVENC reads. Exercise the production bus contract.
        Require(consumer.Release(input.sequence) && producer.IsReleased(input.sequence),
            "Completed GPU source lease was not released");
        Require(packets.size() == 1 && ValidPacket(packets[0]), "GPU input did not immediately produce one valid packet");
        Require(packets[0].sequence == input.sequence && packets[0].timestamp100ns == input.timestamp100ns &&
            packets[0].kind == input.kind, "GPU encoding lost metadata");
        Require(packets[0].displayWidth == input.width / 2 && packets[0].displayHeight == input.height,
            "GPU encoding lost original single-eye aspect");
        if (n < 2) samples[n] = packets[0];
        if (n >= Warmup) {
            durations.push_back(std::chrono::duration<double, std::milli>(after - before).count());
            const auto timing = encoder.lastTiming();
            timings.inputSubmitMs += timing.inputSubmitMs;
            timings.encodeSubmitMs += timing.encodeSubmitMs;
            timings.bitstreamWaitMs += timing.bitstreamWaitMs;
            timings.flushMs += timing.flushMs;
            timings.mapInputMs += timing.mapInputMs;
            timings.encodePictureMs += timing.encodePictureMs;
        }
    }
    const double wallMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::sort(durations.begin(), durations.end());
    const double mean = std::accumulate(durations.begin(), durations.end(), 0.) / durations.size();
    std::cout << CodecName(codec) << " shared D3D12 7680x2160 -> D3D11 VP -> NVENC 3840x2160"
        << " frames=" << Measured << " mean_ms=" << mean << " p95_ms=" << durations[size_t(Measured * .95) - 1]
        << " wall_fps=" << Measured * 1000. / wallMs
        << " input_submit_ms=" << timings.inputSubmitMs / Measured
        << " encode_submit_ms=" << timings.encodeSubmitMs / Measured
        << " bitstream_wait_ms=" << timings.bitstreamWaitMs / Measured
        << " flush_ms=" << timings.flushMs / Measured
        << " map_input_ms=" << timings.mapInputMs / Measured
        << " encode_picture_ms=" << timings.encodePictureMs / Measured << '\n';
    for (unsigned n = 0; n < samples.size(); ++n) {
        Decoder decoder(true); std::vector<Frame> decoded;
        const bool okay = decoder.Decode(samples[n], decoded, error);
        Require(okay && decoder.hardware() && decoded.size() == 1, "NVDEC GPU sample decode: " + error);
        Require(decoded[0].width == config.width && decoded[0].height == config.height &&
            decoded[0].sequence == samples[n].sequence && decoded[0].timestamp100ns == samples[n].timestamp100ns &&
            decoded[0].displayWidth == 3840 && decoded[0].displayHeight == 2160,
            "NVDEC lost GPU packet dimensions or metadata");
        CheckPixels(decoded[0], n);
    }
    std::vector<Frame> held;
    {
        Decoder gpuDecoder(true, &scene.frames[0].adapterLuid);
        for (unsigned n = 0; n < 6; ++n) {
            std::vector<Frame> decoded;
            Require(gpuDecoder.Decode(samples[n % 2], decoded, error) && decoded.size() == 1, "DirectX GPU decode: " + error);
            held.push_back(std::move(decoded[0]));
        }
        for (unsigned n = 0; n < held.size(); ++n) CheckPixels(ReadGpu(held[n]), n % 2);
    }
    // Shared output leases remain readable across decoder reconfiguration/destruction.
    CheckPixels(ReadGpu(held.front()), 0);
    std::cout << "GPU-only decode, DirectX color conversion, held-slot isolation and teardown lifetime PASS\n";
    Require(encoder.RequestKeyFrame(error), "RGBA keyframe: " + error);
    const bool rgbaEncoded = encoder.EncodeGpu(scene.frames[2], packets, error);
    Require(rgbaEncoded && packets.size() == 1, "RGBA shared input: " + error);
    Decoder rgbaDecoder(true); std::vector<Frame> rgbaDecoded;
    const bool rgbaOkay = rgbaDecoder.Decode(packets[0], rgbaDecoded, error);
    Require(rgbaOkay && rgbaDecoded.size() == 1, "RGBA shared decode: " + error);
    CheckPixels(rgbaDecoded[0], 2);
    Frame thumbnail;
    const auto previewDeadline = GetTickCount64() + 1000;
    while (!encoder.GpuPreview(thumbnail) && GetTickCount64() < previewDeadline) Sleep(1);
    Require(ValidFrame(thumbnail) && thumbnail.width <= 432 && thumbnail.height <= 432 &&
        thumbnail.sequence == scene.frames[2].sequence && thumbnail.timestamp100ns == scene.frames[2].timestamp100ns,
        "Nonblocking GPU thumbnail was missing or changed metadata");
    std::cout << CodecName(codec) << " shared resource format, source size, metadata, eye colors, keyframe decode, GPU leases PASS\n";
    // The automatic policy retains AUTO below 4K, even on a multi-engine GPU.
    // The real selected mode and queried engine count are recorded by ConfigureGpu.
    Encoder smaller;
    const CodecConfig smallConfig{1920, 1080, 60, 40000000, {}, codec};
    Require(smaller.ConfigureGpu(smallConfig, scene.frames[0], error, target), "1080p ConfigureGpu: " + error);
    Require(smaller.EncodeGpu(scene.frames[0], packets, error) && packets.size() == 1, "1080p GPU encode: " + error);
    Decoder smallDecoder(true); std::vector<Frame> smallDecoded;
    Require(smallDecoder.Decode(packets[0], smallDecoded, error) && smallDecoder.hardware() && smallDecoded.size() == 1,
        "1080p GPU decode: " + error);
    Require(smallDecoded[0].width == 1920 && smallDecoded[0].height == 1080, "1080p encoded dimensions changed");
    CheckPixels(smallDecoded[0], 0);
    std::cout << CodecName(codec) << " below-4K GPU encode/decode PASS\n";
}
}
int main(int argc, char** argv) {
    try {
        wchar_t splitOption[16]{};
        const auto splitLength = GetEnvironmentVariableW(L"LAN3D_NVENC_SPLIT", splitOption, 16);
        const char* splitLabel = !splitLength ? "unset" : splitLength == 1 && splitOption[0] == L'0' ? "0" :
            splitLength == 1 && splitOption[0] == L'2' ? "2" : "invalid";
        std::cout << "NVENC_GPU_TEST split_override=" << splitLabel << '\n';
        SharedScene scene;
        if (!scene.Initialize()) { std::cout << "SKIP: NVIDIA D3D12 device unavailable\n"; return 77; }
        LUID target{}; bool cross = argc > 1 && std::string(argv[1]) == "--cross";
        if (cross) {
            const auto adapters = EncodeAdapters();
            const auto found = std::find_if(adapters.begin(), adapters.end(), [&](const auto& a) { return !SameAdapter(a.luid, scene.frames[0].adapterLuid); });
            if (found == adapters.end()) { std::cout << "SKIP: second physical NVIDIA GPU unavailable\n"; return 77; }
            target = found->luid;
        }
        Run(scene, CodecId::Av1, cross ? &target : nullptr);
        Run(scene, CodecId::Hevc, cross ? &target : nullptr);
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
