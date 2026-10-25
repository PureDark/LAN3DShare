#pragma once
#include "CaptureApi.h"
#include "common/Frame.h"
#include "common/GpuFrame.h"
#include <d3d11.h>
#include <d3d12.h>
#include <functional>
#include <memory>
#include <string>

namespace lan3d::capture {
struct Diagnostics12 {
    size_t trackedLists{}, confirmedLists{}, recordingSlots{}, retiredSlots{};
    uint64_t submittedLists{}, unmatchedLists{};
};
// Hooks call these methods only at the actual application API boundaries.
// Tests exercise the same GPU paths with WARP and the debug layer.
class Core {
public:
    using FrameSink = std::function<void(const Frame&)>;
    using StatusSink = std::function<void(const std::string&)>;
    using GpuFrameSink = std::function<bool(const GpuFrame&)>;
    using GpuFrameReleased = std::function<bool(uint64_t)>;
    Core(FrameSink frame, StatusSink status);
    ~Core();
    void Configure(const CaptureConfig& config);
    // Callbacks must be nonblocking. A successful publish pins the texture until
    // released(sequence) proves the external GPU reader has finished using it.
    void SetGpuOutput(GpuFrameSink publish, GpuFrameReleased released);
    void SetEnabled(bool enabled);
    uint32_t Register(void* key, HWND window, bool dx12);
    void Remove(void* key);
    void Window(void* key, HWND window);
    bool HasInput(void* key);
    void Input11(void* key, ID3D11ShaderResourceView* view);
    void Input11Target(void* key, ID3D11RenderTargetView* view);
    void Context11(void* key, ID3D11DeviceContext* context, bool onlyIfUnset = false);
    void Input12(void* key, ID3D12Resource* resource, DXGI_FORMAT format, bool onlyIfUnset = false);
    void CommandList12(void* key, ID3D12GraphicsCommandList* list);
    void BeforeWeave(void* key);
    void Present(HWND window);
    void Poll12();
    Diagnostics12 GetDiagnostics12();
    bool OwnsList12(ID3D12CommandList* list);
    void Barrier12(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
    void Invalidate12(ID3D12GraphicsCommandList* list);
    void Reset12(ID3D12GraphicsCommandList* list);
    void ForgetList12(ID3D12GraphicsCommandList* list);
    void ForgetQueue12(ID3D12CommandQueue* queue);
    void Execute12(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                   const std::function<void()>& original);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
