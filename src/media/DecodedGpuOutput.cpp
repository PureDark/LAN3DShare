#include "DecodedGpuOutput.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <atomic>
#include <stdexcept>

namespace lan3d::media {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr, const char* action) { if (FAILED(hr)) { char code[32]; sprintf_s(code, " (0x%08X)", unsigned(hr)); throw std::runtime_error(std::string(action) + code); } }
constexpr char ConvertShader[] = R"(
ByteAddressBuffer nv12 : register(t0);
RWTexture2D<float4> bgra : register(u0);
cbuffer Size : register(b0) { uint width; uint height; uint surfaceHeight; uint padding; };
uint Byte(uint offset) { return (nv12.Load(offset & ~3u) >> ((offset & 3u) * 8)) & 255u; }
[numthreads(16,16,1)] void Main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    float y = float(Byte(p.y * width + p.x)) - 16;
    uint uv = surfaceHeight * width + (p.y / 2) * width + (p.x & ~1u);
    float u = float(Byte(uv)) - 128; float v = float(Byte(uv + 1)) - 128;
    bgra[p.xy] = float4(saturate(float3(298*y+459*v, 298*y-55*u-136*v, 298*y+541*u) / (256*255)), 1);
})";
}
struct DecodedGpuOutput::Slot : GpuImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> view;
    HANDLE handle{};
    ~Slot() { if (handle) CloseHandle(handle); }
};
bool DecodedGpuOutput::Open(LUID adapter, uint32_t width, uint32_t height, std::string& error) {
    try {
        adapter_ = adapter; width_ = width; height_ = height;
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> selected;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "decoder factory");
        Check(factory->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&selected)), "decoder GPU adapter");
        Check(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_), "decoder D3D11 device");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = (width * ((height + 1) & ~1u) * 3 / 2 + 3) & ~3u;
        buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE; buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Check(device_->CreateBuffer(&buffer, nullptr, &input_), "decoder NV12 GPU buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; view.BufferEx.NumElements = buffer.ByteWidth / 4;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device_->CreateShaderResourceView(input_.Get(), &view, &inputView_), "decoder NV12 view");
        const uint32_t size[]{width, height, (height + 1) & ~1u, 0};
        buffer = {}; buffer.ByteWidth = sizeof(size); buffer.Usage = D3D11_USAGE_IMMUTABLE; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{size}; Check(device_->CreateBuffer(&buffer, &data, &constants_), "decoder dimensions");
        ComPtr<ID3DBlob> shader, messages;
        Check(D3DCompile(ConvertShader, sizeof(ConvertShader), nullptr, nullptr, nullptr, "Main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &messages), "decoder DirectX color shader");
        Check(device_->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &shader_), "decoder color shader");
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0}; Check(device_->CreateQuery(&query, &complete_), "decoder completion query");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool DecodedGpuOutput::Convert(Frame& frame, std::string& error) {
    try {
        std::shared_ptr<Slot> slot;
        for (const auto& candidate : slots_) if (candidate.use_count() == 1) { slot = candidate; break; }
        if (!slot) {
            if (slots_.size() >= 8) { frame = {}; return true; } // bounded latest-frame delivery; never overwrite an in-flight reader
            slot = std::make_shared<Slot>();
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = width_; desc.Height = height_;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            Check(device_->CreateTexture2D(&desc, nullptr, &slot->texture), "decoder shared output");
            Check(device_->CreateUnorderedAccessView(slot->texture.Get(), nullptr, &slot->view), "decoder output UAV");
            static std::atomic<uint64_t> ids{};
            const auto name = L"Local\\LAN3DShare.decode." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++ids);
            wcscpy_s(slot->metadata.textureName, name.c_str());
            slot->metadata.width = width_; slot->metadata.height = height_; slot->metadata.format = desc.Format; slot->metadata.adapterLuid = adapter_;
            ComPtr<IDXGIResource1> resource; Check(slot->texture.As(&resource), "decoder shared output resource");
            Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name.c_str(), &slot->handle), "decoder output handle");
            slots_.push_back(slot);
        }
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->CSSetShaderResources(0, 1, inputView_.GetAddressOf());
        context_->CSSetUnorderedAccessViews(0, 1, slot->view.GetAddressOf(), nullptr);
        context_->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
        context_->ClearState(); context_->End(complete_.Get()); context_->Flush();
        const auto deadline = GetTickCount64() + 5000;
        HRESULT hr;
        while ((hr = context_->GetData(complete_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            if (GetTickCount64() > deadline) throw std::runtime_error("Decoder GPU conversion timed out");
            SwitchToThread();
        }
        Check(hr, "decoder GPU conversion completion");
        slot->metadata.sequence = frame.sequence; slot->metadata.timestamp100ns = frame.timestamp100ns; slot->metadata.kind = frame.kind;
        frame.gpu = std::move(slot); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}
