#include "DesktopCapture.h"
#include "DesktopRegion.h"
#include "common/FrameBus.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace lan3d {
using Microsoft::WRL::ComPtr;
namespace {
struct Monitor { HMONITOR handle{}; std::wstring name; };
BOOL CALLBACK AddMonitor(HMONITOR handle, HDC, LPRECT, LPARAM value) {
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(handle, &info)) reinterpret_cast<std::vector<Monitor>*>(value)->push_back({handle, info.szDevice});
    return TRUE;
}
std::vector<Monitor> Monitors() {
    std::vector<Monitor> result;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&result)); return result;
}
std::string Hresult(const char* operation, HRESULT hr) {
    char code[24]{}; sprintf_s(code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(operation) + " failed (" + code + ")";
}
bool Lost(HRESULT hr) {
    return hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_SESSION_DISCONNECTED;
}
float DesktopWhiteScale(const std::wstring& name) {
    UINT32 paths{}, modes{};
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 1.f;
    std::vector<DISPLAYCONFIG_PATH_INFO> active(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, active.data(), &modes, modeInfo.data(), nullptr) != ERROR_SUCCESS) return 1.f;
    for (UINT32 i = 0; i < paths; ++i) {
        const auto& path = active[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || name != source.viewGdiDeviceName) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
        level.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(level), path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel) return 1000.f / level.SDRWhiteLevel;
    }
    return 1.f;
}

// Convert only when the stream needs SDR. The desktop device owns this context.
struct DesktopSdrConverter {
    ComPtr<ID3D11Texture2D> input, output;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    HRESULT Draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
        const D3D11_BOX& region, ID3D11Texture2D* destination, float whiteScale) {
        const UINT width = region.right - region.left, height = region.bottom - region.top;
        HRESULT hr{};
        if (!ps) {
            constexpr char shader[] = R"(
                Texture2D<float4> image : register(t0);
                cbuffer White : register(b0) { float scale; };
                float4 VS(uint i : SV_VertexID) : SV_Position {
                    return float4(float2((i << 1) & 2, i & 2) * float2(2,-2) + float2(-1,1), 0, 1);
                }
                float4 PS(float4 p : SV_Position) : SV_Target {
                    return float4(saturate(image.Load(int3(p.xy,0)).rgb * scale), 1);
                })";
            ComPtr<ID3DBlob> v, p;
            if (FAILED(hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &v, nullptr)) ||
                FAILED(hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &p, nullptr))) return hr;
            if (FAILED(hr = device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs)) ||
                FAILED(hr = device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps))) return hr;
            const float scale[4]{whiteScale};
            D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(scale); buffer.Usage = D3D11_USAGE_IMMUTABLE; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{scale};
            if (FAILED(hr = device->CreateBuffer(&buffer, &initial, &constants))) return hr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        if (input) input->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height || !target) {
            inputView.Reset(); target.Reset(); input.Reset(); output.Reset();
            desc = {}; desc.Width = width; desc.Height = height; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(hr = device->CreateTexture2D(&desc, nullptr, &input)) ||
                FAILED(hr = device->CreateShaderResourceView(input.Get(), nullptr, &inputView))) return hr;
            // The sRGB RTV does the linear-to-sRGB transfer on store.
            desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(hr = device->CreateTexture2D(&desc, nullptr, &output)) ||
                FAILED(hr = device->CreateRenderTargetView(output.Get(), &view, &target))) return hr;
        }
        context->CopySubresourceRegion(input.Get(), 0, 0, 0, 0, source, 0, &region);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, inputView.GetAddressOf()); context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* empty{};
        context->PSSetShaderResources(0, 1, &empty); context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(destination, output.Get());
        return S_OK;
    }
};
}
struct DesktopCapture::Impl {
    struct Slot {
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Query> query;
        uint32_t width{}, height{};
        uint64_t sequence{}, timestamp{};
        bool busy{}, retained{};
        std::shared_ptr<void> sharedHandle;
        GpuFrame metadata;
    };
    std::wstring monitorName;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    std::array<Slot, 3> slots;
    uint64_t sequence{};
    bool gpuMode{}, center4k{}, allowHdr{}, hdrDisplay{};
    float whiteScale{1.f};
    DesktopSdrConverter sdrConverter;
    DXGI_FORMAT loggedSource{DXGI_FORMAT_UNKNOWN}, loggedOutput{DXGI_FORMAT_UNKNOWN};
    Slot* current{};
    LUID adapterLuid{};
    ULONGLONG retryAt{};
    bool opened{}, captured{};
    void ResetGpu() {
        // These are D3D11-owned copies, never a game's device or resources.
        if (context) context->Flush();
        sdrConverter = {}; loggedSource = loggedOutput = DXGI_FORMAT_UNKNOWN;
        current = nullptr; slots = {}; duplication.Reset(); context.Reset(); device.Reset(); captured = false;
    }
    bool Recreate(std::string& error) {
        ResetGpu();
        const auto monitors = Monitors();
        const auto selected = std::find_if(monitors.begin(), monitors.end(), [&](const auto& m) { return m.name == monitorName; });
        if (selected == monitors.end()) { error = "The selected desktop monitor is no longer available"; return false; }
        ComPtr<IDXGIFactory1> factory;
        auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); if (FAILED(hr)) { error = Hresult("CreateDXGIFactory1", hr); return false; }
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput1> output;
        DXGI_OUTPUT_DESC outputDesc{};
        for (UINT adapterIndex = 0; ; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> candidate;
            hr = factory->EnumAdapters1(adapterIndex, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) { error = Hresult("EnumAdapters1", hr); return false; }
            for (UINT outputIndex = 0; ; ++outputIndex) {
                ComPtr<IDXGIOutput> candidateOutput;
                hr = candidate->EnumOutputs(outputIndex, &candidateOutput);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) { error = Hresult("EnumOutputs", hr); return false; }
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(candidateOutput->GetDesc(&desc)) && desc.Monitor == selected->handle && desc.AttachedToDesktop) {
                    hr = candidateOutput.As(&output); if (FAILED(hr)) { error = Hresult("IDXGIOutput1", hr); return false; }
                    adapter = candidate; outputDesc = desc; break;
                }
            }
            if (output) break;
        }
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&adapterDesc))) adapterLuid = adapterDesc.AdapterLuid;
        if (!output) { error = "No DXGI output matches the selected desktop monitor"; return false; }
        if (outputDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY) { error = "Rotated desktop outputs are not supported by this version"; return false; }
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) { error = Hresult("Desktop D3D11CreateDevice", hr); return false; }
        ComPtr<IDXGIOutput6> output6; DXGI_OUTPUT_DESC1 color{};
        hdrDisplay = SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&color)) && color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        whiteScale = hdrDisplay ? DesktopWhiteScale(monitorName) : 1.f;
        ComPtr<IDXGIOutput5> output5;
        if (SUCCEEDED(output.As(&output5))) {
            constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
            hr = output5->DuplicateOutput1(device.Get(), 0, hdrDisplay ? 2 : 1, formats, &duplication);
        } else hr = E_NOINTERFACE;
        // Some HDR drivers expose Output5 but reject an explicit format list.
        // Inspect the actual texture from the legacy entry point in that case.
        if (hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED || hr == E_INVALIDARG)
            hr = output->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(hr)) { error = Hresult("DuplicateOutput", hr); ResetGpu(); return false; }
        DXGI_OUTDUPL_DESC desc{}; duplication->GetDesc(&desc);
        // The output above establishes orientation. Some drivers leave the
        // duplication rotation unspecified. Validate format on the acquired texture.
        if (desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED && desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
            error = "Unsupported desktop duplication rotation " + std::to_string(desc.Rotation); ResetGpu(); return false;
        }
        const auto region = SelectDesktopRegion(desc.ModeDesc.Width, desc.ModeDesc.Height, center4k);
        const auto width = region.width, height = region.height;
        if (width < 4 || (width & 1) || width > MaxWidth || height < 2 || height > MaxHeight || uint64_t(width) * height * 4 > MaxFrameBytes) {
            error = "Desktop dimensions exceed the supported even-width video frame limits"; ResetGpu(); return false;
        }
        error.clear(); return true;
    }
    DesktopCaptureResult PollReady(Frame* cpu, GpuFrame* gpu, std::string& error) {
        Slot* newest{};
        for (auto& slot : slots) {
            if (!slot.busy) continue;
            const auto hr = context->GetData(slot.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            if (FAILED(hr)) { error = Hresult("Desktop GPU copy query", hr); return DesktopCaptureResult::Error; }
            if (!newest || newest->sequence < slot.sequence) { if (newest) newest->busy = false; newest = &slot; }
            else slot.busy = false;
        }
        if (!newest) return DesktopCaptureResult::NoChange;
        if (gpu) {
            if (current) current->retained = false;
            current = newest; newest->retained = true; newest->busy = false;
            newest->metadata.sequence = newest->sequence; newest->metadata.timestamp100ns = newest->timestamp;
            *gpu = newest->metadata; return DesktopCaptureResult::FrameReady;
        }
        auto& frame = *cpu;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr = context->Map(newest->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return DesktopCaptureResult::NoChange;
        if (FAILED(hr)) { error = Hresult("Desktop staging Map", hr); return DesktopCaptureResult::Error; }
        struct Unmap { ID3D11DeviceContext* context; ID3D11Resource* resource; ~Unmap() { context->Unmap(resource, 0); } } unmap{context.Get(), newest->staging.Get()};
        frame.width = newest->width; frame.height = newest->height; frame.stride = newest->width * 4;
        frame.sequence = newest->sequence; frame.timestamp100ns = newest->timestamp; frame.kind = ContentKind::Desktop;
        frame.bgra.resize(size_t(frame.stride) * frame.height);
        for (uint32_t y = 0; y < frame.height; ++y) std::memcpy(frame.bgra.data() + size_t(y) * frame.stride,
            static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, frame.stride);
        newest->busy = false; return DesktopCaptureResult::FrameReady;
    }
};
DesktopCapture::DesktopCapture() : p_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() = default;
bool DesktopCapture::Open(unsigned monitorIndex, std::string& error, bool center4k) {
    Close(); error.clear(); const auto monitors = Monitors();
    if (monitorIndex >= monitors.size()) { error = "Desktop monitor index is out of range"; return false; }
    p_->monitorName = monitors[monitorIndex].name;
    p_->center4k = center4k;
    p_->opened = p_->Recreate(error); return p_->opened;
}
void DesktopCapture::AllowHdr(bool enabled) {
    if (p_->allowHdr != enabled) { p_->allowHdr = enabled; RequestRefresh(); }
}
void DesktopCapture::Close() { p_->ResetGpu(); p_->opened = false; p_->monitorName.clear(); p_->sequence = 0; p_->retryAt = 0; }
void DesktopCapture::RequestRefresh() {
    if (!p_->opened) return;
    p_->ResetGpu(); p_->retryAt = 0;
}
bool DesktopCapture::CanRepeatCurrent() const {
    return p_->duplication && p_->captured &&
        std::none_of(p_->slots.begin(), p_->slots.end(), [](const auto& slot) { return slot.busy; });
}
DesktopCaptureResult DesktopCapture::Capture(Frame& frame, std::string& error) { return CaptureInternal(&frame, nullptr, error); }
DesktopCaptureResult DesktopCapture::CaptureGpu(GpuFrame& frame, std::string& error) { return CaptureInternal(nullptr, &frame, error); }
DesktopCaptureResult DesktopCapture::CaptureInternal(Frame* cpu, GpuFrame* gpu, std::string& error) {
    if (p_->gpuMode != (gpu != nullptr)) { p_->ResetGpu(); p_->gpuMode = gpu != nullptr; }
    error.clear();
    if (!p_->opened) { error = "Desktop capture is not open"; return DesktopCaptureResult::Error; }
    if (!p_->duplication) {
        if (GetTickCount64() < p_->retryAt) return DesktopCaptureResult::NoChange;
        if (!p_->Recreate(error)) { p_->retryAt = GetTickCount64() + 500; return DesktopCaptureResult::NoChange; }
    }
    auto ready = p_->PollReady(cpu, gpu, error);
    if (ready == DesktopCaptureResult::Error) { p_->ResetGpu(); p_->retryAt = GetTickCount64() + 250; return DesktopCaptureResult::NoChange; }
    auto idle = std::find_if(p_->slots.begin(), p_->slots.end(), [](const auto& slot) { return !slot.busy && !slot.retained; });
    if (idle == p_->slots.end()) return ready;
    DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
    const auto acquired = p_->duplication->AcquireNextFrame(0, &info, &resource);
    if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
        if (gpu && ready == DesktopCaptureResult::NoChange && p_->current && CanRepeatCurrent()) {
            *gpu = p_->current->metadata; gpu->timestamp100ns = Timestamp100ns(); gpu->sequence = ++p_->sequence;
            return DesktopCaptureResult::FrameReady;
        }
        return ready;
    }
    if (FAILED(acquired)) {
        error = Hresult("AcquireNextFrame", acquired);
        if (Lost(acquired)) { p_->ResetGpu(); p_->retryAt = GetTickCount64() + 250; return gpu ? DesktopCaptureResult::NoChange : ready; }
        return ready == DesktopCaptureResult::FrameReady ? ready : DesktopCaptureResult::Error;
    }
    struct ReleaseFrame { IDXGIOutputDuplication* duplication; ~ReleaseFrame() { duplication->ReleaseFrame(); } } release{p_->duplication.Get()};
    if (p_->captured && info.LastPresentTime.QuadPart == 0) return ready; // Cursor-only update; cursor composition is not implemented.
    ComPtr<ID3D11Texture2D> texture;
    auto hr = resource.As(&texture); if (FAILED(hr)) { error = Hresult("Desktop image Texture2D", hr); return DesktopCaptureResult::Error; }
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    const auto region = SelectDesktopRegion(desc.Width, desc.Height, p_->center4k);
    desc.Width = region.width; desc.Height = region.height;
    const auto sourceFormat = desc.Format;
    const bool floating = sourceFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((!floating && sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM) || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width < 4 || (desc.Width & 1) || desc.Width > MaxWidth || desc.Height < 2 || desc.Height > MaxHeight || uint64_t(desc.Width) * desc.Height * 4 > MaxFrameBytes) {
        error = "Unsupported desktop duplication image (DXGI format " + std::to_string(desc.Format) + ", " +
            std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ")"; return DesktopCaptureResult::Error;
    }
    const bool convertToSdr = floating && !(gpu && p_->hdrDisplay && p_->allowHdr);
    if (convertToSdr) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (p_->loggedSource != sourceFormat || p_->loggedOutput != desc.Format) {
        printf("DESKTOP_COLOR source_dxgi=%u hdr_display=%u output=%s white_scale=%.3f\n", unsigned(sourceFormat),
            unsigned(p_->hdrDisplay), desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "scRGB" : "SDR", p_->whiteScale);
        fflush(stdout); p_->loggedSource = sourceFormat; p_->loggedOutput = desc.Format;
    }
    if (!idle->staging || idle->width != desc.Width || idle->height != desc.Height || idle->metadata.format != desc.Format) {
        idle->staging.Reset(); idle->query.Reset(); idle->sharedHandle.reset();
        auto staging = desc;
        staging.Usage = gpu ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = gpu ? 0 : D3D11_CPU_ACCESS_READ;
        staging.BindFlags = gpu ? D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET : 0;
        staging.MiscFlags = gpu ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0;
        hr = p_->device->CreateTexture2D(&staging, nullptr, &idle->staging);
        if (FAILED(hr)) { error = Hresult("Desktop staging allocation", hr); return DesktopCaptureResult::Error; }
        if (gpu) {
            static std::atomic<uint64_t> names{};
            idle->metadata = {}; idle->metadata.width = desc.Width; idle->metadata.height = desc.Height;
            idle->metadata.format = desc.Format; idle->metadata.kind = ContentKind::Desktop; idle->metadata.adapterLuid = p_->adapterLuid;
            const auto name = L"Local\\LAN3DShare.desktop." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++names);
            wcscpy_s(idle->metadata.textureName, name.c_str());
            ComPtr<IDXGIResource1> resource1; HANDLE handle{};
            hr = idle->staging.As(&resource1);
            if (SUCCEEDED(hr)) hr = resource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name.c_str(), &handle);
            if (FAILED(hr)) { error = Hresult("Desktop shared handle", hr); idle->staging.Reset(); return DesktopCaptureResult::Error; }
            idle->sharedHandle = std::shared_ptr<void>(handle, [](void* h) { CloseHandle(h); });
        }
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0}; hr = p_->device->CreateQuery(&query, &idle->query);
        if (FAILED(hr)) { error = Hresult("Desktop copy query allocation", hr); idle->staging.Reset(); return DesktopCaptureResult::Error; }
        idle->metadata.format = desc.Format;
        idle->width = desc.Width; idle->height = desc.Height;
    }
    const D3D11_BOX box{region.x, region.y, 0, region.x + region.width, region.y + region.height, 1};
    if (convertToSdr) {
        hr = p_->sdrConverter.Draw(p_->device.Get(), p_->context.Get(), texture.Get(), box, idle->staging.Get(), p_->whiteScale);
        if (FAILED(hr)) { error = Hresult("Desktop FP16 to SDR conversion", hr); return DesktopCaptureResult::Error; }
    } else p_->context->CopySubresourceRegion(idle->staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
    p_->context->End(idle->query.Get()); p_->context->Flush();
    idle->sequence = ++p_->sequence; idle->timestamp = Timestamp100ns(); idle->busy = true; p_->captured = true;
    return ready;
}
}
