#include "Renderer.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace lan3d::receiver {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        char text[160]{};
        sprintf_s(text, "%s failed (0x%08lX)", action, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
constexpr char PreviewShader[] = R"(
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex o; o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
Texture2D image : register(t0); SamplerState linearClamp : register(s0);
float4 PS(Vertex i) : SV_Target { return image.SampleLevel(linearClamp, i.uv, 0); }
float4 PSLeft(Vertex i) : SV_Target { return image.SampleLevel(linearClamp, float2(i.uv.x * .5, i.uv.y), 0); }
)";
}
Renderer::~Renderer() {
    // A runtime may own GPU resources and command callbacks. Finish GPU work
    // before unloading its DLLs, including the bridge's renderer implementation.
    if (dc_) dc_->ClearState();
    DrainGpu();
    runtime_.DestroyWeaver();
    DrainGpu();
    runtime_.Close();
}
void Renderer::Open(HWND window, HMONITOR monitor, bool preview, const std::filesystem::path& runtime, bool autoDisplay) {
    window_ = window; preview_ = preview; autoDisplay_ = autoDisplay; runtimeDirectory_ = runtime;
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> selected;
    for (UINT i = 0; !selected; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT j = 0; ; ++j) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) { selected = adapter; break; }
        }
    }
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    auto hr = D3D11CreateDevice(selected.Get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
        &device_, nullptr, &dc_);
    if (FAILED(hr) && preview)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &dc_);
    Check(hr, "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory2;
    Check(device_.As(&dxgiDevice), "Query DXGI device");
    Check(dxgiDevice->GetAdapter(&adapter), "Get DXGI adapter");
    Check(adapter->GetParent(IID_PPV_ARGS(&factory2)), "Get DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    Check(factory2->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swap_), "Create swap chain");
    factory2->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGIDevice1> dxgi1;
    if (SUCCEEDED(device_.As(&dxgi1))) dxgi1->SetMaximumFrameLatency(1);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    Check(device_->CreateRasterizerState(&raster, &raster_), "Create rasterizer");
    {
        ComPtr<ID3DBlob> vs, ps, left, error;
        Check(D3DCompile(PreviewShader, sizeof(PreviewShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &error), "Compile preview vertex shader");
        Check(D3DCompile(PreviewShader, sizeof(PreviewShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &error), "Compile preview pixel shader");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_), "Create preview vertex shader");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_), "Create preview pixel shader");
        Check(D3DCompile(PreviewShader, sizeof(PreviewShader), nullptr, nullptr, nullptr, "PSLeft", "ps_5_0", 0, 0, &left, &error), "Compile left-eye preview shader");
        Check(device_->CreatePixelShader(left->GetBufferPointer(), left->GetBufferSize(), nullptr, &pixelLeft_), "Create left-eye preview shader");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sampler, &sampler_), "Create preview sampler");
    }
    Resize();
}
void Renderer::Resize() {
    RECT rect{};
    GetClientRect(window_, &rect);
    const auto width = unsigned(rect.right), height = unsigned(rect.bottom);
    if (!width || !height || (width == width_ && height == height_)) return;
    dc_->OMSetRenderTargets(0, nullptr, nullptr);
    target_.Reset();
    Check(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize swap chain");
    ComPtr<ID3D11Texture2D> backbuffer;
    Check(swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Get backbuffer");
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Check(device_->CreateRenderTargetView(backbuffer.Get(), &rtv, &target_), "Create sRGB backbuffer view");
    width_ = width; height_ = height;
}
LUID Renderer::AdapterLuid() const {
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
    Check(device_.As(&dxgi), "renderer DXGI device"); Check(dxgi->GetAdapter(&adapter), "renderer adapter");
    Check(adapter->GetDesc(&desc), "renderer adapter description"); return desc.AdapterLuid;
}
void Renderer::Upload(const Frame& frame) {
    if (!ValidVideoFrame(frame)) throw std::runtime_error("Decoder supplied an invalid packed SBS frame");
    kind_ = frame.kind;
    displayWidth_ = frame.displayWidth ? frame.displayWidth : (kind_ == ContentKind::LeiaSbs ? frame.width / 2 : frame.width);
    displayHeight_ = frame.displayHeight ? frame.displayHeight : frame.height;
    if (kind_ != ContentKind::LeiaSbs && runtimeActive_) DisableRuntime();
    const auto format = frame.gpu && frame.gpu->metadata.format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (frame.width != inputWidth_ || frame.height != inputHeight_ || format != inputFormat_) {
        ID3D11ShaderResourceView* nullView{};
        dc_->PSSetShaderResources(0, 1, &nullView);
        inputView_.Reset(); input_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame.width; desc.Height = frame.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&desc, nullptr, &input_), "Create received SBS texture");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
        Check(device_->CreateShaderResourceView(input_.Get(), &view, &inputView_), "Create received SBS view");
        inputWidth_ = frame.width; inputHeight_ = frame.height; inputFormat_ = format;
        if (runtimeActive_) runtime_.SetInput(device_.Get(), dc_.Get(), window_, inputView_.Get(), frame.width, frame.height);
    }
    std::erase_if(gpuReads_, [&](const auto& read) { return dc_->GetData(read.complete.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK; });
    if (frame.gpu) {
        const auto& metadata = frame.gpu->metadata;
        auto found = sharedInputs_.find(metadata.textureName);
        if (found == sharedInputs_.end()) {
            if (sharedInputs_.size() >= 24) sharedInputs_.clear();
            ComPtr<ID3D11Device1> device; Check(device_.As(&device), "renderer shared device");
            ComPtr<ID3D11Texture2D> source;
            Check(device->OpenSharedResourceByName(metadata.textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&source)), "open decoded GPU image");
            found = sharedInputs_.emplace(metadata.textureName, std::move(source)).first;
        }
        GpuRead read; read.lease = frame.gpu;
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0}; Check(device_->CreateQuery(&query, &read.complete), "decoded GPU read fence");
        dc_->CopyResource(input_.Get(), found->second.Get()); dc_->End(read.complete.Get()); dc_->Flush();
        gpuReads_.push_back(std::move(read));
    } else {
    dc_->UpdateSubresource(input_.Get(), 0, nullptr, frame.bgra.data(), frame.stride, 0);
    const size_t middleRow = size_t(frame.height / 2) * frame.stride;
    memcpy(verifyLeft_, frame.bgra.data() + middleRow + size_t(frame.width / 4) * 4, 4);
    memcpy(verifyRight_, frame.bgra.data() + middleRow + size_t(frame.width * 3 / 4) * 4, 4);
    }
    if (!preview_ && !autoDisplay_ && kind_ == ContentKind::LeiaSbs) {
        std::string error;
        if (!SetFullscreen(true, error)) throw std::runtime_error(error);
    }
}
bool Renderer::Thumbnail(const Frame& metadata, Frame& thumbnail) {
    try {
    if (!input_) return false;
    if (thumbnailPending_) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(dc_->Map(thumbnailReadback_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) return false;
        thumbnail = thumbnailMetadata_;
        thumbnail.bgra.resize(size_t(thumbnail.stride) * thumbnail.height);
        for (uint32_t y = 0; y < thumbnail.height; ++y)
            memcpy(thumbnail.bgra.data() + size_t(y) * thumbnail.stride, static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, thumbnail.stride);
        dc_->Unmap(thumbnailReadback_.Get(), 0); thumbnailPending_ = false; return true;
    }
    const auto now = GetTickCount64();
    if (now - lastThumbnail_ < 1000) return false;
    lastThumbnail_ = now;
    const double scale = std::min({1.0, 432.0 / metadata.width, 432.0 / metadata.height});
    const auto width = uint32_t(metadata.width * scale) & ~1u, height = uint32_t(metadata.height * scale) & ~1u;
    if (!thumbnailTarget_ || thumbnailMetadata_.width != width || thumbnailMetadata_.height != height) {
        thumbnailView_.Reset(); thumbnailTarget_.Reset(); thumbnailReadback_.Reset();
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device_->CreateTexture2D(&desc, nullptr, &thumbnailTarget_), "thumbnail GPU texture");
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(device_->CreateRenderTargetView(thumbnailTarget_.Get(), &view, &thumbnailView_), "thumbnail GPU output");
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device_->CreateTexture2D(&desc, nullptr, &thumbnailReadback_), "thumbnail readback");
    }
    thumbnailMetadata_ = {}; thumbnailMetadata_.width = width; thumbnailMetadata_.height = height; thumbnailMetadata_.stride = width * 4;
    thumbnailMetadata_.sequence = metadata.sequence; thumbnailMetadata_.timestamp100ns = metadata.timestamp100ns; thumbnailMetadata_.kind = metadata.kind;
    thumbnailMetadata_.displayWidth = metadata.displayWidth; thumbnailMetadata_.displayHeight = metadata.displayHeight;
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1}; D3D11_RECT scissor{0, 0, LONG(width), LONG(height)};
    dc_->OMSetRenderTargets(1, thumbnailView_.GetAddressOf(), nullptr); dc_->OMSetBlendState(nullptr, nullptr, ~0u); dc_->OMSetDepthStencilState(nullptr, 0);
    dc_->RSSetViewports(1, &viewport); dc_->RSSetScissorRects(1, &scissor); dc_->RSSetState(raster_.Get());
    dc_->IASetInputLayout(nullptr); dc_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc_->VSSetShader(vertex_.Get(), nullptr, 0); dc_->PSSetShader(pixel_.Get(), nullptr, 0);
    dc_->PSSetShaderResources(0, 1, inputView_.GetAddressOf()); dc_->PSSetSamplers(0, 1, sampler_.GetAddressOf()); dc_->Draw(3, 0);
    ID3D11ShaderResourceView* empty{}; dc_->PSSetShaderResources(0, 1, &empty); dc_->OMSetRenderTargets(0, nullptr, nullptr);
    dc_->CopyResource(thumbnailReadback_.Get(), thumbnailTarget_.Get()); dc_->Flush(); thumbnailPending_ = true;
    return false;
    } catch (...) { thumbnailPending_ = false; return false; }
}
void Renderer::DisableRuntime() noexcept {
    if (dc_) dc_->ClearState();
    DrainGpu();
    runtime_.DestroyWeaver();
    DrainGpu();
    runtime_.Close();
    runtimeActive_ = false;
}
void Renderer::SetStereoEnabled(bool enabled) {
    stereoEnabled_ = enabled;
    if (!enabled && runtimeActive_) DisableRuntime();
}
bool Renderer::SetFullscreen(bool fullscreen, std::string& error) {
    error.clear();
    const bool wantsWeave = stereoEnabled_ && !preview_ && fullscreen && kind_ == ContentKind::LeiaSbs && input_;
    if (!wantsWeave) { if (runtimeActive_) DisableRuntime(); return true; }
    if (runtimeActive_) return true;
    try {
        runtime_.Open(runtimeDirectory_);
        runtime_.SetInput(device_.Get(), dc_.Get(), window_, inputView_.Get(), inputWidth_, inputHeight_);
        runtimeActive_ = true;
        return true;
    } catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "LeiaSR runtime failed to initialize"; }
    DisableRuntime();
    return false;
}
bool Renderer::Draw(bool verifyPreview) {
    lastPresentSucceeded_ = false;
    std::erase_if(gpuReads_, [&](const auto& read) { return dc_->GetData(read.complete.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK; });
    Resize();
    if (!target_) return false;
    D3D11_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
    const D3D11_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    dc_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
    dc_->RSSetViewports(1, &viewport); dc_->RSSetScissorRects(1, &scissor);
    dc_->RSSetState(raster_.Get());
    const float clear[]{0.015f, 0.015f, 0.015f, 1};
    dc_->ClearRenderTargetView(target_.Get(), clear);
    bool ready = false;
    if (input_) {
        if (!runtimeActive_) {
            const bool leftEye = (!preview_ || !stereoEnabled_) && kind_ == ContentKind::LeiaSbs;
            const float imageWidth = float(displayWidth_ * (kind_ == ContentKind::LeiaSbs && !leftEye ? 2u : 1u));
            const float scale = (std::min)(float(width_) / imageWidth, float(height_) / float(displayHeight_));
            D3D11_VIEWPORT imageViewport{(float(width_) - imageWidth * scale) * .5f,
                (float(height_) - float(displayHeight_) * scale) * .5f, imageWidth * scale, float(displayHeight_) * scale, 0, 1};
            dc_->RSSetViewports(1, &imageViewport);
            dc_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            dc_->OMSetDepthStencilState(nullptr, 0);
            dc_->IASetInputLayout(nullptr);
            dc_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dc_->VSSetShader(vertex_.Get(), nullptr, 0);
            dc_->PSSetShader(leftEye ? pixelLeft_.Get() : pixel_.Get(), nullptr, 0);
            dc_->PSSetShaderResources(0, 1, inputView_.GetAddressOf());
            dc_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
            dc_->Draw(3, 0);
            ID3D11ShaderResourceView* nullView{};
            dc_->PSSetShaderResources(0, 1, &nullView);
            if (verifyPreview) VerifyPreview();
            ready = true;
        } else ready = runtime_.Weave(width_, height_);
    }
    const HRESULT hr = swap_->Present(1, 0);
    Check(hr, "Present");
    lastPresentSucceeded_ = ready && hr == S_OK && IsWindowVisible(window_) && !IsIconic(window_);
    if (hr == DXGI_STATUS_OCCLUDED) Sleep(20);
    return ready;
}
void Renderer::VerifyPreview() {
    ComPtr<ID3D11Texture2D> backbuffer, staging;
    Check(swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Get verification backbuffer");
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device_->CreateTexture2D(&desc, nullptr, &staging), "Create verification readback");
    dc_->CopyResource(staging.Get(), backbuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(dc_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map verification readback");
    const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(height_ / 2) * mapped.RowPitch;
    bool matches = true;
    const auto* expectedRight = (!preview_ || !stereoEnabled_) && kind_ == ContentKind::LeiaSbs ? verifyLeft_ : verifyRight_;
    const float aspect = float(displayWidth_ * (preview_ && stereoEnabled_ && kind_ == ContentKind::LeiaSbs ? 2u : 1u)) / displayHeight_;
    const float imageWidth = (std::min)(float(width_), float(height_) * aspect);
    const float left = (float(width_) - imageWidth) * .5f;
    // Diagnostic patterns are uniform inside each eye. Include positions near
    // both image edges so an incorrectly squeezed half-SBS viewport fails too.
    for (const float fraction : {.125f, .25f, .75f, .875f}) {
        const auto x = (std::min)(size_t(left + imageWidth * fraction), size_t(width_ - 1));
        const auto* expected = fraction < .5f ? verifyLeft_ : expectedRight;
        for (unsigned c = 0; c < 3; ++c)
            matches &= abs(int(row[x * 4 + c]) - int(expected[c])) <= 2;
    }
    dc_->Unmap(staging.Get(), 0);
    if (!matches) throw std::runtime_error("Preview readback failed: display aspect, SBS eye order or sRGB round-trip differs");
}
void Renderer::DrainGpu() noexcept {
    if (!device_ || !dc_) return;
    ComPtr<ID3D11Query> query;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    if (FAILED(device_->CreateQuery(&desc, &query))) { dc_->Flush(); return; }
    dc_->End(query.Get()); dc_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (dc_->GetData(query.Get(), nullptr, 0, 0) == S_FALSE && std::chrono::steady_clock::now() < deadline) Sleep(1);
}
}
