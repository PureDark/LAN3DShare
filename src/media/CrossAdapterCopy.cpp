#include "CrossAdapterCopy.h"
#include <d3d12.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <map>
#include <stdexcept>
#include <cstdio>

namespace lan3d::media {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    char code[32]{}; sprintf_s(code, " (0x%08X)", unsigned(hr));
    throw std::runtime_error(std::string(operation) + code);
}
struct Handle { HANDLE value{}; ~Handle() { if (value) CloseHandle(value); } };
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &b);
}
}
struct CrossAdapterCopy::Impl {
    ComPtr<ID3D12Device> device[2];
    ComPtr<ID3D12CommandQueue> queue[2];
    ComPtr<ID3D12CommandAllocator> allocator[2];
    ComPtr<ID3D12GraphicsCommandList> list[2];
    ComPtr<ID3D12Resource> shared[2], target;
    ComPtr<ID3D12Heap> heap[2];
    ComPtr<ID3D12QueryHeap> timestamps[2];
    ComPtr<ID3D12Resource> timingReadback[2];
    uint64_t frequency[2]{};
    ComPtr<ID3D12Fence> ready[2], done;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DeviceContext4> consumerContext;
    ComPtr<ID3D11Fence> consumerFence;
    std::map<std::wstring, ComPtr<ID3D12Resource>> sources;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    GpuFrame description{};
    Handle event;
    uint64_t serial{};
    ~Impl() {
        // No timeout-based resource reuse: even a failed call retains its
        // resources until the GPU completes or reports device removal.
        if (done && serial && done->GetCompletedValue() < serial && event.value &&
            SUCCEEDED(done->SetEventOnCompletion(serial, event.value))) WaitForSingleObject(event.value, INFINITE);
    }
};
CrossAdapterCopy::CrossAdapterCopy() : p_(std::make_unique<Impl>()) {}
CrossAdapterCopy::~CrossAdapterCopy() = default;
bool CrossAdapterCopy::Open(const GpuFrame& source, ID3D11Device* destination, std::string& error) {
    p_ = std::make_unique<Impl>(); auto& p = *p_;
    try {
        if (!ValidGpuFrame(source) || !destination) throw std::runtime_error("Invalid cross-adapter image");
        p.description = source;
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "cross-adapter factory");
        ComPtr<IDXGIAdapter1> adapter; Check(factory->EnumAdapterByLuid(source.adapterLuid, IID_PPV_ARGS(&adapter)), "source adapter");
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p.device[0])), "source D3D12 device");
        ComPtr<IDXGIDevice> dxgi; Check(destination->QueryInterface(IID_PPV_ARGS(&dxgi)), "destination DXGI device");
        ComPtr<IDXGIAdapter> dst; Check(dxgi->GetAdapter(&dst), "destination adapter");
        Check(D3D12CreateDevice(dst.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p.device[1])), "destination D3D12 device");
        for (unsigned i = 0; i != 2; ++i) {
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            Check(p.device[i]->CreateCommandQueue(&q, IID_PPV_ARGS(&p.queue[i])), "create COPY queue");
            Check(p.device[i]->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&p.allocator[i])), "COPY allocator");
            Check(p.device[i]->CreateCommandList(0, q.Type, p.allocator[i].Get(), nullptr, IID_PPV_ARGS(&p.list[i])), "COPY list");
            Check(p.list[i]->Close(), "close COPY list");
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 features{};
            if (SUCCEEDED(p.device[i]->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &features, sizeof(features))) && features.CopyQueueTimestampQueriesSupported) {
                D3D12_QUERY_HEAP_DESC queries{}; queries.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP; queries.Count = 2;
                Check(p.device[i]->CreateQueryHeap(&queries, IID_PPV_ARGS(&p.timestamps[i])), "COPY timestamp query");
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK; hp.CreationNodeMask = hp.VisibleNodeMask = 1;
                D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 16; rd.Height = 1;
                rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                Check(p.device[i]->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.timingReadback[i])), "COPY timing counters");
                Check(p.queue[i]->GetTimestampFrequency(&p.frequency[i]), "COPY timestamp frequency");
            }
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = source.width; td.Height = source.height;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = source.format;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Check(destination->CreateTexture2D(&td, nullptr, &p.texture), "destination local texture");
        ComPtr<IDXGIResource1> resource; Check(p.texture.As(&resource), "destination shared resource");
        Handle textureHandle;
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &textureHandle.value), "destination handle");
        Check(p.device[1]->OpenSharedHandle(textureHandle.value, IID_PPV_ARGS(&p.target)), "open destination texture on COPY device");
        const auto desc = p.target->GetDesc(); UINT64 bytes{};
        p.device[0]->GetCopyableFootprints(&desc, 0, 1, 0, &p.footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        D3D12_HEAP_DESC heap{}; heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.Properties.CreationNodeMask = heap.Properties.VisibleNodeMask = 1;
        heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        heap.SizeInBytes = p.device[0]->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
        Check(p.device[0]->CreateHeap(&heap, IID_PPV_ARGS(&p.heap[0])), "cross-adapter GPU heap");
        Handle sharedHandle, fenceHandle;
        Check(p.device[0]->CreateSharedHandle(p.heap[0].Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle.value), "cross-adapter handle");
        Check(p.device[1]->OpenSharedHandle(sharedHandle.value, IID_PPV_ARGS(&p.heap[1])), "open cross-adapter heap");
        for (unsigned i = 0; i != 2; ++i) Check(p.device[i]->CreatePlacedResource(p.heap[i].Get(), 0, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.shared[i])), "cross-adapter placed buffer");
        Check(p.device[0]->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&p.ready[0])), "cross-adapter fence");
        Check(p.device[0]->CreateSharedHandle(p.ready[0].Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle.value), "cross-adapter fence handle");
        Check(p.device[1]->OpenSharedHandle(fenceHandle.value, IID_PPV_ARGS(&p.ready[1])), "open cross-adapter fence");
        Check(p.device[1]->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&p.done)), "destination completion fence");
        Handle consumerHandle;
        Check(p.device[1]->CreateSharedHandle(p.done.Get(), nullptr, GENERIC_ALL, nullptr, &consumerHandle.value), "destination completion handle");
        ComPtr<ID3D11Device5> consumerDevice; Check(destination->QueryInterface(IID_PPV_ARGS(&consumerDevice)), "D3D11 GPU fence device");
        Check(consumerDevice->OpenSharedFence(consumerHandle.value, IID_PPV_ARGS(&p.consumerFence)), "open destination fence on encoder device");
        ComPtr<ID3D11DeviceContext> context; destination->GetImmediateContext(&context);
        Check(context.As(&p.consumerContext), "D3D11 GPU wait context");
        p.event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p.event.value) throw std::runtime_error("Cannot create cross-adapter completion event");
        error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
CrossAdapterCopy::Timing CrossAdapterCopy::CompletedTiming() const {
    Timing result; const auto& p = *p_;
    if (!p.serial || !p.done || p.done->GetCompletedValue() < p.serial) return result;
    for (unsigned i = 0; i != 2; ++i) if (p.timingReadback[i] && p.frequency[i]) {
        const D3D12_RANGE read{0, 16}; uint64_t* counters{};
        if (SUCCEEDED(p.timingReadback[i]->Map(0, &read, reinterpret_cast<void**>(&counters)))) {
            const auto ms = double(counters[1] - counters[0]) * 1000.0 / p.frequency[i];
            (i == 0 ? result.sourceMs : result.destinationMs) = ms;
            const D3D12_RANGE written{0, 0}; p.timingReadback[i]->Unmap(0, &written);
        }
    }
    return result;
}
bool CrossAdapterCopy::Copy(const GpuFrame& source, ID3D11Texture2D** output, std::string& error) {
    auto& p = *p_; *output = nullptr;
    try {
        if (!p.target || source.width != p.description.width || source.height != p.description.height ||
            source.format != p.description.format || source.adapterLuid.LowPart != p.description.adapterLuid.LowPart ||
            source.adapterLuid.HighPart != p.description.adapterLuid.HighPart) throw std::runtime_error("Cross-adapter source changed; reconfigure required");
        auto found = p.sources.find(source.textureName);
        if (found == p.sources.end()) {
            if (p.sources.size() >= 24) p.sources.clear();
            Handle handle; Check(p.device[0]->OpenSharedHandleByName(source.textureName, GENERIC_ALL, &handle.value), "open source texture name");
            ComPtr<ID3D12Resource> input; Check(p.device[0]->OpenSharedHandle(handle.value, IID_PPV_ARGS(&input)), "open source texture on COPY device");
            const auto d = input->GetDesc();
            if (d.Width != source.width || d.Height != source.height || d.Format != source.format || d.MipLevels != 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1)
                throw std::runtime_error("Cross-adapter source metadata mismatch");
            found = p.sources.emplace(source.textureName, std::move(input)).first;
        }
        // The caller completes its NVENC read before submitting another frame.
        // Refuse allocator reuse if that ownership contract is violated.
        if (p.serial && p.done->GetCompletedValue() < p.serial) throw std::runtime_error("Cross-adapter input still in flight");
        for (unsigned i = 0; i != 2; ++i) {
            Check(p.allocator[i]->Reset(), "reset COPY allocator");
            Check(p.list[i]->Reset(p.allocator[i].Get(), nullptr), "reset COPY list");
            ID3D12Resource* texture = i == 0 ? found->second.Get() : p.target.Get();
            const auto state = i == 0 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
            if (p.timestamps[i]) p.list[i]->EndQuery(p.timestamps[i].Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            Barrier(p.list[i].Get(), texture, D3D12_RESOURCE_STATE_COMMON, state);
            D3D12_TEXTURE_COPY_LOCATION image{}; image.pResource = texture; image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION buffer{}; buffer.pResource = p.shared[i].Get(); buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; buffer.PlacedFootprint = p.footprint;
            p.list[i]->CopyTextureRegion(i == 0 ? &buffer : &image, 0, 0, 0, i == 0 ? &image : &buffer, nullptr);
            Barrier(p.list[i].Get(), texture, state, D3D12_RESOURCE_STATE_COMMON);
            if (p.timestamps[i]) {
                p.list[i]->EndQuery(p.timestamps[i].Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                p.list[i]->ResolveQueryData(p.timestamps[i].Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, p.timingReadback[i].Get(), 0);
            }
            Check(p.list[i]->Close(), "close COPY submission");
        }
        const auto value = p.serial + 1;
        ID3D12CommandList* from[]{p.list[0].Get()}; p.queue[0]->ExecuteCommandLists(1, from);
        Check(p.queue[0]->Signal(p.ready[0].Get(), value), "source COPY signal");
        Check(p.queue[1]->Wait(p.ready[1].Get(), value), "destination COPY wait");
        ID3D12CommandList* to[]{p.list[1].Get()}; p.queue[1]->ExecuteCommandLists(1, to);
        Check(p.queue[1]->Signal(p.done.Get(), value), "destination COPY signal"); p.serial = value;
        // Queue the dependency on the consumer GPU. The CPU immediately
        // continues submitting VP/NVENC; no CPU wait between the COPY engines
        // and conversion. NVENC completion covers the entire dependency chain.
        Check(p.consumerContext->Wait(p.consumerFence.Get(), value), "encoder GPU wait for COPY");
        Check(p.device[0]->GetDeviceRemovedReason(), "source COPY device"); Check(p.device[1]->GetDeviceRemovedReason(), "destination COPY device");
        *output = p.texture.Get(); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}
