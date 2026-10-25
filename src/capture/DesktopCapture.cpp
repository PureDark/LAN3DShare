#include "DesktopCapture.h"
#include "common/FrameBus.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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
    bool gpuMode{};
    Slot* current{};
    LUID adapterLuid{};
    ULONGLONG retryAt{};
    bool opened{}, captured{};
    void ResetGpu() {
        // These are D3D11-owned copies, never a game's device or resources.
        if (context) context->Flush();
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
        hr = output->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(hr)) { error = Hresult("DuplicateOutput", hr); ResetGpu(); return false; }
        DXGI_OUTDUPL_DESC desc{}; duplication->GetDesc(&desc);
        if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
            error = "Desktop duplication requires unrotated BGRA8 output"; ResetGpu(); return false;
        }
        const auto width = desc.ModeDesc.Width, height = desc.ModeDesc.Height;
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
bool DesktopCapture::Open(unsigned monitorIndex, std::string& error) {
    Close(); error.clear(); const auto monitors = Monitors();
    if (monitorIndex >= monitors.size()) { error = "Desktop monitor index is out of range"; return false; }
    p_->monitorName = monitors[monitorIndex].name;
    p_->opened = p_->Recreate(error); return p_->opened;
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
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.Width < 4 || (desc.Width & 1) || desc.Width > MaxWidth || desc.Height < 2 || desc.Height > MaxHeight || uint64_t(desc.Width) * desc.Height * 4 > MaxFrameBytes) {
        error = "Unsupported desktop duplication image format or dimensions"; return DesktopCaptureResult::Error;
    }
    if (!idle->staging || idle->width != desc.Width || idle->height != desc.Height) {
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
        idle->width = desc.Width; idle->height = desc.Height;
    }
    p_->context->CopyResource(idle->staging.Get(), texture.Get()); p_->context->End(idle->query.Get()); p_->context->Flush();
    idle->sequence = ++p_->sequence; idle->timestamp = Timestamp100ns(); idle->busy = true; p_->captured = true;
    return ready;
}
}
