#include "capture/CaptureCore.h"
#include <d3d11sdklayers.h>
#include <d3d11_1.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <set>
#include <cstring>

using Microsoft::WRL::ComPtr;
using lan3d::capture::Core;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Hr(HRESULT hr, const char* message) { if (FAILED(hr)) throw std::runtime_error(message); }
std::vector<uint8_t> Pixels(unsigned w, unsigned h, uint8_t red, uint8_t blue) {
    std::vector<uint8_t> bytes(size_t(w) * h * 4);
    for (size_t i = 0; i < bytes.size(); i += 4) { bytes[i] = red; bytes[i + 1] = 71; bytes[i + 2] = blue; bytes[i + 3] = 255; }
    return bytes;
}
void CheckPixels(const lan3d::Frame& frame, unsigned w, unsigned h, uint8_t red, uint8_t blue) {
    Check(frame.width == w && frame.height == h && frame.stride == w * 4, "wrong SBS view dimensions");
    for (size_t i = 0; i < frame.bgra.size(); i += 4) Check(frame.bgra[i] == blue && frame.bgra[i + 1] == 71 && frame.bgra[i + 2] == red && frame.bgra[i + 3] == 255, "SBS pixels or RGBA-to-BGRA conversion incorrect");
}
void Debug11(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> q; Hr(device->QueryInterface(IID_PPV_ARGS(&q)), "DX11 debug-layer unavailable");
    for (uint64_t i = 0; i < q->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; q->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size); auto m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); q->GetMessage(i, m, &size);
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
    }
}
void Debug12(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> q; Hr(device->QueryInterface(IID_PPV_ARGS(&q)), "DX12 debug-layer unavailable");
    for (uint64_t i = 0; i < q->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; q->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size); auto m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); q->GetMessage(i, m, &size);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
    }
}
void Wait12(ID3D12Device* d, ID3D12CommandQueue* q) {
    ComPtr<ID3D12Fence> f; Hr(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "fence"); Hr(q->Signal(f.Get(), 1), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(event != nullptr, "event");
    Hr(f->SetEventOnCompletion(1, event), "completion"); const auto result = WaitForSingleObject(event, 5000); CloseHandle(event); Check(result == WAIT_OBJECT_0, "GPU timeout");
}
void CheckGpuPixels(ID3D11Device* device, ID3D11DeviceContext* context, const lan3d::GpuFrame& frame, uint8_t red, uint8_t blue) {
    Check(lan3d::ValidGpuFrame(frame), "invalid GPU metadata");
    ComPtr<ID3D11Device1> device1; Hr(device->QueryInterface(IID_PPV_ARGS(&device1)), "DX11.1 shared consumer");
    ComPtr<ID3D11Texture2D> texture;
    Hr(device1->OpenSharedResourceByName(frame.textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&texture)), "open GPU texture by NT name");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    Check(desc.Width == frame.width && desc.Height == frame.height && desc.Format == frame.format, "shared texture metadata mismatch");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.BindFlags = desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> readback; Hr(device->CreateTexture2D(&desc, nullptr, &readback), "GPU test readback");
    context->CopyResource(readback.Get(), texture.Get()); context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map GPU test readback");
    bool valid = true;
    for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
        const auto p = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
        valid &= p[0] == red && p[1] == 71 && p[2] == blue && p[3] == 255;
    }
    context->Unmap(readback.Get(), 0); Check(valid, "shared GPU texture pixels changed or incorrect");
}
void Test11() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "DX11 WARP debug device");
    std::vector<lan3d::Frame> frames; std::string status;
    Core core([&](const auto& f) { frames.push_back(f); }, [&](const auto& s) { status = s; });
    lan3d::CaptureConfig config; config.maxFps = 0; core.Configure(config); core.SetEnabled(true);
    int key{}; const auto window = reinterpret_cast<HWND>(0x1234); core.Register(&key, window, false); core.Context11(&key, context.Get());
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 16; desc.Height = 8; desc.MipLevels = 2; desc.ArraySize = 2; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> input; Hr(device->CreateTexture2D(&desc, nullptr, &input), "DX11 input");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = desc.Format; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray = {1, 1, 1, 1}; ComPtr<ID3D11ShaderResourceView> srv; Hr(device->CreateShaderResourceView(input.Get(), &view, &srv), "DX11 SRV"); core.Input11(&key, srv.Get());
    auto mask = Pixels(8, 4, 11, 23), final = Pixels(8, 4, 203, 37);
    context->UpdateSubresource(input.Get(), 3, nullptr, mask.data(), 32, 0); core.BeforeWeave(&key);
    context->UpdateSubresource(input.Get(), 3, nullptr, final.data(), 32, 0); core.BeforeWeave(&key);
    Check(frames.empty(), "published a mask before Present selection"); core.Present(window); context->Flush();
    for (unsigned n = 0; n < 500 && frames.empty(); ++n) { core.Present(window); Sleep(1); }
    Check(frames.size() == 1, "DX11 final candidate missing"); CheckPixels(frames.back(), 8, 4, 203, 37);
    config.weaveOrdinal = 1; core.Configure(config);
    context->UpdateSubresource(input.Get(), 3, nullptr, mask.data(), 32, 0); core.BeforeWeave(&key);
    context->UpdateSubresource(input.Get(), 3, nullptr, final.data(), 32, 0); core.BeforeWeave(&key); core.Present(window); context->Flush();
    for (unsigned n = 0; n < 500 && frames.size() < 2; ++n) { core.Present(window); Sleep(1); }
    Check(frames.size() == 2, "explicit weave selection missing"); CheckPixels(frames.back(), 8, 4, 11, 23);
    ComPtr<ID3D11DeviceContext> deferred; Hr(device->CreateDeferredContext(0, &deferred), "deferred"); core.Context11(&key, deferred.Get());
    core.BeforeWeave(&key); core.Present(window); Check(frames.size() == 2 && status.find("deferred") != std::string::npos, "deferred context not rejected");
    // Auto selection follows a recreated weaver; an explicitly selected stream
    // number remains unchanged and must never silently switch to a different one.
    core.Remove(&key); int replacementKey{}; core.Register(&replacementKey, window, false);
    core.Context11(&replacementKey, context.Get()); core.Input11(&replacementKey, srv.Get());
    core.BeforeWeave(&replacementKey); core.Present(window); context->Flush();
    for (unsigned n = 0; n < 500 && frames.size() < 3; ++n) { core.Present(window); Sleep(1); }
    Check(frames.size() == 3, "auto stream failed after weaver recreation"); CheckPixels(frames.back(), 8, 4, 203, 37);
    std::vector<lan3d::GpuFrame> gpuFrames; std::set<uint64_t> released;
    core.SetGpuOutput([&](const auto& f) { gpuFrames.push_back(f); return true; }, [&](uint64_t token) { return released.contains(token); });
    config.reserved = lan3d::CaptureGpuOutput; core.Configure(config);
    for (unsigned n = 0; n < 6; ++n) {
        const auto sample = Pixels(8, 4, static_cast<uint8_t>(100 + n), 55);
        context->UpdateSubresource(input.Get(), 3, nullptr, sample.data(), 32, 0); core.BeforeWeave(&replacementKey); core.Present(window); context->Flush();
        for (unsigned poll = 0; poll < 500 && gpuFrames.size() <= n; ++poll) { core.Present(window); Sleep(1); }
        Check(gpuFrames.size() == n + 1, "DX11 GPU capture missing");
    }
    CheckGpuPixels(device.Get(), context.Get(), gpuFrames.front(), 100, 55);
    core.BeforeWeave(&replacementKey); core.Present(window); context->Flush();
    Check(gpuFrames.size() == 6 && status.find("ring full") != std::string::npos, "DX11 overwrote an outstanding GPU consumer lease");
    CheckGpuPixels(device.Get(), context.Get(), gpuFrames.front(), 100, 55);
    released.insert(gpuFrames.front().sequence);
    core.BeforeWeave(&replacementKey); core.Present(window); context->Flush();
    for (unsigned poll = 0; poll < 500 && gpuFrames.size() < 7; ++poll) { core.Present(window); Sleep(1); }
    Check(gpuFrames.size() == 7 && wcscmp(gpuFrames[0].textureName, gpuFrames[6].textureName) == 0, "DX11 did not reuse the released shared texture");
    for (const auto& f : gpuFrames) released.insert(f.sequence);
    config.maxFps = 1; core.Configure(config);
    for (unsigned n = 0; n < 50; ++n) { core.BeforeWeave(&replacementKey); core.Present(window); }
    context->Flush();
    for (unsigned poll = 0; poll < 500 && gpuFrames.size() < 8; ++poll) { core.Present(window); Sleep(1); }
    Check(gpuFrames.size() == 8, "FPS limit was not applied before GPU allocation/copy");
    for (const auto& f : gpuFrames) released.insert(f.sequence);
    core.Present(window); core.SetEnabled(false); core.Remove(&replacementKey);
    Check(frames.size() == 3, "GPU mode performed a CPU pixel readback");
    Debug11(device.Get()); std::cout << "DX11: same-texture mask/final selection, SRV mip/slice, RGBA conversion, explicit ordinal, deferred rejection, debug layer PASS\n";
    std::cout << "DX11 GPU: NT sharing, exact pixels, consumer lease backpressure/reuse, upstream FPS limit, no CPU frame callback PASS\n";
}
// Genuine COM wrapper; the weaver and the actual queue see different pointers.
class ForwardList12 final : public ID3D12GraphicsCommandList {
public:
    ComPtr<ID3D12GraphicsCommandList> native;
    bool forwardPrivate{true};
    ULONG references{1};
    GUID privateGuid{};
    std::vector<uint8_t> privateBytes;
    explicit ForwardList12(ID3D12GraphicsCommandList* value) : native(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
            iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList)) {
            *out = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (forwardPrivate) return native->GetPrivateData(guid, size, data);
        if (guid != privateGuid || !size || *size < privateBytes.size()) return E_INVALIDARG;
        *size = static_cast<UINT>(privateBytes.size()); if (data) std::memcpy(data, privateBytes.data(), *size); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        privateGuid = guid; privateBytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return forwardPrivate ? native->SetPrivateData(guid, size, data) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override { return native->SetPrivateDataInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return native->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return native->GetDevice(iid, out); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return native->GetType(); }
    HRESULT STDMETHODCALLTYPE Close(void) override { return native->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) override { return native->Reset(pAllocator, pInitialState); }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pPipelineState) override { return native->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override { return native->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override { return native->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override { return native->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override { return native->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes); }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox) override { return native->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox); }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) override { return native->CopyResource(pDstResource, pSrcResource); }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override { return native->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override { return native->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override { return native->IASetPrimitiveTopology(PrimitiveTopology); }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) override { return native->RSSetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects) override { return native->RSSetScissorRects(NumRects, pRects); }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[ 4 ]) override { return native->OMSetBlendFactor(BlendFactor); }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override { return native->OMSetStencilRef(StencilRef); }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pPipelineState) override { return native->SetPipelineState(pPipelineState); }
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers) override { return native->ResourceBarrier(NumBarriers, pBarriers); }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *pCommandList) override { return native->ExecuteBundle(pCommandList); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps) override { return native->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps); }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *pRootSignature) override { return native->SetComputeRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) override { return native->SetGraphicsRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { return native->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { return native->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override { return native->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override { return native->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override { return native->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override { return native->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { return native->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) override { return native->IASetIndexBuffer(pView); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews) override { return native->IASetVertexBuffers(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override { return native->SOSetTargets(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override { return native->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects) override { return native->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override { return native->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override { return native->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override { return native->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) override { return native->DiscardResource(pResource, pRegion); }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override { return native->BeginQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override { return native->EndQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource *pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override { return native->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset); }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override { return native->SetPredication(pBuffer, AlignedBufferOffset, Operation); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override { return native->SetMarker(Metadata, pData, Size); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override { return native->BeginEvent(Metadata, pData, Size); }
    void STDMETHODCALLTYPE EndEvent(void) override { return native->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset) override { return native->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset); }
};

void Test12() {
    ComPtr<ID3D12Debug> debug; Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "DX12 debug-layer unavailable"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); ComPtr<IDXGIAdapter> adapter; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "DX12 WARP device");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator, replacement; Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "replacement allocator");
    ComPtr<ID3D12GraphicsCommandList> list; Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    ForwardList12 wrapper(list.Get());
    Check(static_cast<ID3D12GraphicsCommandList*>(&wrapper) != list.Get(), "wrapper and submitted interface unexpectedly identical");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 8; desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; ComPtr<ID3D12Resource> input;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&input)), "input12");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t bytes{}; device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload; Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload12");
    void* data{}; Hr(upload->Map(0, nullptr, &data), "map upload"); const auto pixels = Pixels(8, 4, 207, 34);
    for (unsigned y = 0; y < 4; ++y) memcpy(static_cast<uint8_t*>(data) + size_t(y) * footprint.Footprint.RowPitch, pixels.data() + size_t(y) * 32, 32); upload->Unmap(0, nullptr);
    std::vector<lan3d::Frame> frames; std::string status;
    Core core([&](const auto& f) { frames.push_back(f); }, [&](const auto& s) { status = s; }); lan3d::CaptureConfig config; config.maxFps = 0; core.Configure(config); core.SetEnabled(true);
    int key{}; auto window = reinterpret_cast<HWND>(0x5678); core.Register(&key, window, true); core.Input12(&key, input.Get(), desc.Format); core.CommandList12(&key, &wrapper);
    core.BeforeWeave(&key); Check(status.find("state unknown") != std::string::npos && frames.empty(), "unknown DX12 resource state not rejected");
    Check(core.GetDiagnostics12().recordingSlots == 0 && core.GetDiagnostics12().confirmedLists == 0, "unsubmitted list gained capture eligibility");
    Hr(list->Close(), "close identity warmup"); ID3D12CommandList* commandLists[]{list.Get()};
    core.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get());
    Check(core.GetDiagnostics12().confirmedLists == 1, "forwarded PrivateData did not match the actual submitted native list");
    ForwardList12 anotherWrapper(list.Get()); core.CommandList12(&key, &anotherWrapper);
    Check(core.GetDiagnostics12().trackedLists == 1, "new wrapper replaced the existing underlying list token");
    Hr(allocator->Reset(), "identity allocator reset"); Hr(list->Reset(allocator.Get(), nullptr), "identity warmup reset"); core.Reset12(list.Get());
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {input.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = input.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint; list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
    core.BeforeWeave(&key); Hr(list->Close(), "close");
    ComPtr<ID3D12Fence> gate; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate"); Hr(queue->Wait(gate.Get(), 1), "queue gate");
    core.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); });
    core.Present(window); core.Poll12(); Check(frames.empty(), "published DX12 before GPU completion");
    // Reset the list while its old recording is still executing, using another
    // allocator. The old readback must survive until the queue fence completes.
    Hr(list->Reset(replacement.Get(), nullptr), "reset with replacement allocator"); core.Reset12(list.Get());
    Hr(gate->Signal(1), "release gate"); Wait12(device.Get(), queue.Get()); core.Poll12();
    Check(frames.size() == 1, "retired DX12 recording lost frame"); CheckPixels(frames.back(), 8, 4, 207, 34);
    // Record another candidate, then execute this recording twice. Its readback
    // remains owned by the recording, and no duplicate video frame is emitted.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
    core.BeforeWeave(&key); Hr(list->Close(), "close second");
    core.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); core.Present(window); Wait12(device.Get(), queue.Get()); core.Poll12();
    Check(frames.size() == 2, "second DX12 recording missing");
    core.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get()); core.Poll12();
    Check(frames.size() == 2, "re-executed list emitted duplicate token");
    Hr(replacement->Reset(), "allocator reset"); Hr(list->Reset(replacement.Get(), nullptr), "final reset"); core.Reset12(list.Get()); Hr(list->Close(), "final close");
    core.Remove(&key);
    // A weaver destroyed before Present leaves no future selection boundary.
    // Repeating beyond the readback cap catches undecided retired-slot leaks.
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        Hr(replacement->Reset(), "cycle allocator reset"); Hr(list->Reset(replacement.Get(), nullptr), "cycle reset"); core.Reset12(list.Get());
        core.Register(&key, window, true); core.Input12(&key, input.Get(), desc.Format); core.CommandList12(&key, list.Get());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); core.Barrier12(list.Get(), 1, &barrier);
        core.BeforeWeave(&key); Check(status.find("at limit") == std::string::npos, "destroy-before-Present leaked retired readbacks"); Hr(list->Close(), "cycle close");
        core.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); });
        core.Remove(&key); Wait12(device.Get(), queue.Get()); core.Poll12();
    }
    Check(frames.size() == 2, "destroyed stream published an undecided candidate");
    Hr(replacement->Reset(), "cleanup allocator"); Hr(list->Reset(replacement.Get(), nullptr), "cleanup reset"); core.Reset12(list.Get()); Hr(list->Close(), "cleanup close"); core.ForgetQueue12(queue.Get());
    std::vector<uint8_t> previousIdentity;
    {
        Core previous([](const auto&) {}, [](const auto&) {}); ForwardList12 previousWrapper(list.Get());
        previous.Register(&key, window, true); previous.CommandList12(&key, &previousWrapper); previousIdentity = previousWrapper.privateBytes;
    }
    {
        Core quarantine([&](const auto&) { throw std::runtime_error("unobserved submission published"); }, [&](const auto& message) { status = message; });
        ForwardList12 source(list.Get()); quarantine.Configure(config); quarantine.SetEnabled(true);
        quarantine.Register(&key, window, true); quarantine.Input12(&key, input.Get(), desc.Format); quarantine.CommandList12(&key, &source);
        Check(source.privateBytes != previousIdentity, "list token reused across Core lifetimes");
        quarantine.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get());
        Hr(replacement->Reset(), "quarantine allocator"); Hr(list->Reset(replacement.Get(), nullptr), "quarantine recording"); quarantine.Reset12(list.Get());
        const auto transition = [&] {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier); quarantine.Barrier12(&source, 1, &barrier);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); quarantine.Barrier12(&source, 1, &barrier);
        };
        transition(); quarantine.BeforeWeave(&key); Hr(list->Close(), "unobserved recording close");
        Check(quarantine.GetDiagnostics12().recordingSlots == 1, "qualified wrapper failed to record a readback");
        // Deliberately execute real GPU work without the Core submission hook.
        // Its readback must survive Reset while the GPU is still gated.
        Hr(queue->Wait(gate.Get(), 2), "unobserved GPU gate"); queue->ExecuteCommandLists(1, commandLists); quarantine.Present(window);
        Hr(allocator->Reset(), "quarantine replacement allocator"); Hr(list->Reset(allocator.Get(), nullptr), "unobserved in-flight Reset"); quarantine.Reset12(list.Get());
        quarantine.SetEnabled(false); quarantine.Remove(&key); quarantine.Poll12();
        Check(quarantine.GetDiagnostics12().retiredSlots == 1, "unobserved in-flight readback released at Reset/Stop/destroy");
        Hr(gate->Signal(2), "release unobserved gate"); Wait12(device.Get(), queue.Get()); quarantine.Poll12();
        Check(quarantine.GetDiagnostics12().retiredSlots == 1, "missing completion proof was treated as a completed fence");
        quarantine.SetEnabled(true); quarantine.Register(&key, window, true); quarantine.Input12(&key, input.Get(), desc.Format); quarantine.CommandList12(&key, &source);
        for (unsigned i = 0; i < 30; ++i) {
            transition(); quarantine.BeforeWeave(&key); Hr(list->Close(), "discarded recording close");
            Hr(allocator->Reset(), "discarded allocator Reset"); Hr(list->Reset(allocator.Get(), nullptr), "discarded list Reset"); quarantine.Reset12(list.Get());
        }
        Check(quarantine.GetDiagnostics12().retiredSlots == 24 && status.find("at limit") != std::string::npos, "unknown recordings were not held under a bounded quarantine");
        Hr(list->Close(), "quarantine cleanup close");
        // All real work has completed; remaining recordings were never submitted.
    }
    {
        Core unsupported([](const auto&) { throw std::runtime_error("non-forwarding wrapper published"); }, [&](const auto& message) { status = message; });
        ForwardList12 isolated(list.Get()); isolated.forwardPrivate = false;
        unsupported.Configure(config); unsupported.SetEnabled(true); unsupported.Register(&key, window, true);
        unsupported.Input12(&key, input.Get(), desc.Format); unsupported.CommandList12(&key, &isolated);
        unsupported.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get());
        Hr(allocator->Reset(), "isolated allocator"); Hr(list->Reset(allocator.Get(), nullptr), "isolated recording"); unsupported.Reset12(&isolated);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier); unsupported.Barrier12(&isolated, 1, &barrier);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); unsupported.Barrier12(&isolated, 1, &barrier);
        unsupported.BeforeWeave(&key);
        const auto diagnostic = unsupported.GetDiagnostics12();
        Check(diagnostic.confirmedLists == 0 && diagnostic.recordingSlots == 0 && diagnostic.unmatchedLists == 1 && status.find("submission identity pending") != std::string::npos,
            "non-forwarding wrapper was accepted without a matching submitted token");
        Hr(list->Close(), "isolated cleanup close");
    }
    {
        std::vector<lan3d::GpuFrame> gpuFrames; std::set<uint64_t> released;
        Core gpu([](const auto&) { throw std::runtime_error("GPU mode performed a CPU pixel readback"); }, [&](const auto& message) { status = message; });
        gpu.SetGpuOutput([&](const auto& f) { gpuFrames.push_back(f); return true; }, [&](uint64_t token) { return released.contains(token); });
        config.reserved = lan3d::CaptureGpuOutput; gpu.Configure(config); gpu.SetEnabled(true);
        gpu.Register(&key, window, true); gpu.Input12(&key, input.Get(), desc.Format); gpu.CommandList12(&key, list.Get());
        gpu.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get());
        const auto reset = [&](ID3D12CommandAllocator* target) {
            Hr(target->Reset(), "GPU test allocator reset"); Hr(list->Reset(target, nullptr), "GPU test list reset"); gpu.Reset12(list.Get());
        };
        const auto record = [&] {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier); gpu.Barrier12(list.Get(), 1, &barrier);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); gpu.Barrier12(list.Get(), 1, &barrier);
            gpu.BeforeWeave(&key); Hr(list->Close(), "GPU test close");
            Check(gpu.GetDiagnostics12().recordingSlots == 1, "DX12 shared texture copy was not recorded");
            gpu.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); gpu.Present(window);
        };
        ComPtr<ID3D11Device> consumer; ComPtr<ID3D11DeviceContext> consumerContext;
        Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &consumerContext), "DX12 shared DX11 WARP consumer");
        reset(allocator.Get()); record(); Wait12(device.Get(), queue.Get()); gpu.Poll12();
        Check(gpuFrames.empty(), "DX12 published a recording which could still be re-executed");
        gpu.Execute12(queue.Get(), 1, commandLists, [&] { queue->ExecuteCommandLists(1, commandLists); }); Wait12(device.Get(), queue.Get()); gpu.Poll12();
        Check(gpuFrames.empty(), "DX12 re-execution bypassed retirement requirement");
        reset(allocator.Get()); Check(gpuFrames.size() == 1, "DX12 retired GPU texture was not published");
        CheckGpuPixels(consumer.Get(), consumerContext.Get(), gpuFrames[0], 207, 34);
        // A second capture must not overwrite the first consumer's outstanding
        // lease. Even Reset cannot publish while its queue fence is incomplete.
        ComPtr<ID3D12Fence> gpuGate; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuGate)), "GPU shared gate");
        Hr(queue->Wait(gpuGate.Get(), 1), "GPU shared queue gate"); record();
        reset(replacement.Get()); gpu.Poll12();
        Check(gpuFrames.size() == 1, "DX12 published a retired texture before its GPU copy completed");
        Hr(gpuGate->Signal(1), "GPU shared release gate"); Wait12(device.Get(), queue.Get()); gpu.Poll12();
        Check(gpuFrames.size() == 2 && wcscmp(gpuFrames[0].textureName, gpuFrames[1].textureName) != 0, "DX12 reused a leased texture");
        Check(gpu.GetDiagnostics12().retiredSlots == 2, "DX12 did not retain outstanding consumer leases");
        CheckGpuPixels(consumer.Get(), consumerContext.Get(), gpuFrames[0], 207, 34);
        released.insert(gpuFrames[0].sequence); gpu.Poll12();
        record(); Wait12(device.Get(), queue.Get()); reset(replacement.Get());
        Check(gpuFrames.size() == 3 && wcscmp(gpuFrames[0].textureName, gpuFrames[2].textureName) == 0, "DX12 did not reuse a completed and released GPU slot");
        CheckGpuPixels(consumer.Get(), consumerContext.Get(), gpuFrames[2], 207, 34);
        for (unsigned n = 0; n < 22; ++n) { record(); Wait12(device.Get(), queue.Get()); reset(replacement.Get()); }
        Check(gpu.GetDiagnostics12().retiredSlots == 24, "DX12 lease retention count incorrect");
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier); gpu.Barrier12(list.Get(), 1, &barrier);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); gpu.Barrier12(list.Get(), 1, &barrier);
        gpu.BeforeWeave(&key); gpu.Present(window);
        Check(gpu.GetDiagnostics12().recordingSlots == 0 && status.find("at limit") != std::string::npos, "DX12 consumer backpressure was not bounded");
        gpu.SetEnabled(false); gpu.Remove(&key); gpu.Poll12();
        Check(gpu.GetDiagnostics12().retiredSlots == 24, "DX12 Stop/destroy freed a leased shared texture");
        for (const auto& f : gpuFrames) released.insert(f.sequence);
        gpu.Poll12(); Check(gpu.GetDiagnostics12().retiredSlots == 0, "DX12 completed GPU leases leaked");
        Hr(list->Close(), "GPU shared cleanup close"); Debug11(consumer.Get());
    }
    Debug12(device.Get()); std::cout << "DX12: unknown-state rejection, copy/state restoration, GPU-gated publish, in-flight Reset retirement, repeated Execute lifetime, debug layer PASS\n";
    std::cout << "DX12 GPU: shared texture opened on DX11, exact pixels, Reset+fence publication, re-execution exclusion, consumer lease and pooled reuse, no CPU callback PASS\n";
}
int main() { try { Test11(); Test12(); return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
