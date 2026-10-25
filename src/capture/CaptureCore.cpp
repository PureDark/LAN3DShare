#include "CaptureCore.h"
#include "common/FrameBus.h"
#include <wrl/client.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace lan3d::capture {
using Microsoft::WRL::ComPtr;
namespace {
// Public per-object metadata crosses transparent COM wrappers without reading
// their private fields. A token is trusted only after an actual queue submit
// exposes it on the submitted list. Non-forwarding wrappers remain unsupported.
constexpr GUID ListIdentityGuid{0x752b7eca, 0x9d20, 0x48b0, {0xb1, 0x8c, 0x77, 0x34, 0xb6, 0xea, 0xa4, 0x53}};
std::atomic<uint64_t> nextListIdentity{1};
struct ListIdentity {
    uint64_t owner{}, serial{};
    bool operator==(const ListIdentity&) const = default;
};
ListIdentity NewListIdentity() {
    return {reinterpret_cast<uintptr_t>(&nextListIdentity), nextListIdentity.fetch_add(1)};
}
bool ReadListIdentity(ID3D12GraphicsCommandList* list, ListIdentity& identity) {
    UINT size = sizeof(identity);
    return SUCCEEDED(list->GetPrivateData(ListIdentityGuid, &size, &identity)) && size == sizeof(identity);
}
bool Supported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
bool Rgba(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
DXGI_FORMAT LinearFormat(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}
std::atomic<uint64_t> nextSharedSurface{1};
struct SharedSurface {
    HANDLE handle{};
    LUID adapter{};
    std::wstring name;
    SharedSurface() {
        LARGE_INTEGER stamp{}; QueryPerformanceCounter(&stamp);
        name = L"Local\\LAN3DShare.texture." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(stamp.QuadPart) + L"." + std::to_wstring(nextSharedSurface.fetch_add(1));
    }
    ~SharedSurface() { if (handle) CloseHandle(handle); }
};
bool Dimensions(uint64_t w, uint32_t h) { return w >= 4 && !(w & 1) && w <= MaxWidth && h >= 2 && h <= MaxHeight; }
Frame ReadPixels(const void* data, uint32_t pitch, uint32_t w, uint32_t h, DXGI_FORMAT format, uint64_t token, uint64_t timestamp) {
    Frame f; f.width = w; f.height = h; f.stride = w * 4; f.sequence = token; f.timestamp100ns = timestamp;
    f.bgra.resize(size_t(f.stride) * h);
    for (uint32_t y = 0; y < h; ++y) {
        auto dst = f.bgra.data() + size_t(y) * f.stride;
        std::memcpy(dst, static_cast<const uint8_t*>(data) + size_t(y) * pitch, f.stride);
        if (Rgba(format)) for (uint32_t x = 0; x < w; ++x) std::swap(dst[x * 4], dst[x * 4 + 2]);
    }
    return f;
}
}

struct Core::Impl {
    struct Candidate {
        uint64_t token{}, timestamp{}, epoch{};
        uint32_t ordinal{};
        bool decided{}, selected{}, published{};
    };
    struct Slot11 : Candidate {
        ComPtr<ID3D11Texture2D> readback;
        std::shared_ptr<SharedSurface> shared;
        ComPtr<ID3D11Query> query;
        ComPtr<ID3D11DeviceContext> context;
        uint32_t width{}, height{};
        DXGI_FORMAT format{};
        bool busy{};
        bool sent{};
    };
    struct Slot12 : Candidate {
        ComPtr<ID3D12Resource> readback;
        std::shared_ptr<SharedSurface> shared;
        // Keep the input alive while this command recording can be executed.
        ComPtr<ID3D12Resource> source;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint32_t width{}, height{};
        DXGI_FORMAT format{};
        struct Completion { ComPtr<ID3D12Fence> fence; uint64_t value{}; };
        std::vector<Completion> completions;
        bool submitted{}, failedSignal{}, sent{};
        uint64_t bytes{};
        uint32_t streamId{};
        bool Ready() const {
            return submitted && !failedSignal && std::all_of(completions.begin(), completions.end(), [](const auto& c) {
                const auto value = c.fence->GetCompletedValue();
                return value != UINT64_MAX && value >= c.value;
            });
        }
    };
    struct Stream {
        uint32_t id{};
        HWND window{};
        bool dx12{};
        uint64_t epoch{1}, lastToken{}, chosenToken{}, lastSendTime{}, nextCaptureTime{};
        uint32_t ordinal{};
        bool captureEpoch{};
        uint64_t identityStatusCount{UINT64_MAX};
        ComPtr<ID3D11ShaderResourceView> view11;
        ComPtr<ID3D11RenderTargetView> target11;
        ComPtr<ID3D11DeviceContext> context11;
        bool context11Observed{};
        ComPtr<ID3D12Resource> input12;
        DXGI_FORMAT format12{DXGI_FORMAT_UNKNOWN};
        ID3D12GraphicsCommandList* list12{};
        std::array<Slot11, 6> slots11;
        std::string error;
    };
    struct List {
        // Only transitions observed in this recording establish state. We never
        // infer state from resource flags, the Leia runtime version, or a delay.
        std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> state;
        std::vector<std::shared_ptr<Slot12>> slots;
        ListIdentity identity;
        std::set<ID3D12GraphicsCommandList*> aliases;
        bool identityWritten{}, submissionObserved{};
    };
    struct Queue { ComPtr<ID3D12Fence> fence; uint64_t value{}; };
    std::recursive_mutex mutex;
    std::map<void*, Stream> streams;
    std::map<uint64_t, List> lists;
    std::map<ID3D12GraphicsCommandList*, uint64_t> listAliases;
    std::map<ID3D12CommandQueue*, Queue> queues;
    std::vector<std::shared_ptr<Slot12>> retired;
    std::vector<std::shared_ptr<Slot12>> reusable12;
    // In-flight DX11 copies keep their context and staging allocation after a
    // weaver is destroyed. They are polled only on the rendering thread.
    std::vector<Slot11> retired11;
    FrameSink frame;
    StatusSink status;
    GpuFrameSink gpuFrame;
    GpuFrameReleased gpuReleased;
    CaptureConfig config;
    uint32_t nextStream{1}, defaultStream{};
    uint64_t token{}, lastPublished{};
    uint64_t submittedLists{}, unmatchedLists{};
    ID3D12CommandList* lastSubmittedList{};
    bool enabled{};
    Impl(FrameSink f, StatusSink s) : frame(std::move(f)), status(std::move(s)) {}
    void RegisterList(ID3D12GraphicsCommandList* object) {
        if (!object || listAliases.contains(object)) return;
        if (FindList(object)) return;
        const auto identity = NewListIdentity();
        auto& list = lists[identity.serial]; list.identity = identity; list.aliases.insert(object);
        listAliases[object] = identity.serial;
        ListIdentity readback{};
        list.identityWritten = SUCCEEDED(object->SetPrivateData(ListIdentityGuid, sizeof(identity), &identity)) &&
            ReadListIdentity(object, readback) && readback == identity;
    }
    List* FindList(ID3D12GraphicsCommandList* object) {
        if (!object) return nullptr;
        ListIdentity identity{};
        if (!ReadListIdentity(object, identity)) return nullptr;
        const auto found = lists.find(identity.serial);
        if (found == lists.end() || !found->second.identityWritten || found->second.identity != identity) return nullptr;
        auto& list = found->second;
        // Token rebinding invalidates the old alias without releasing its
        // recordings: only Reset/destruction plus completion can retire them.
        if (const auto alias = listAliases.find(object); alias != listAliases.end() && alias->second != identity.serial) {
            if (const auto old = lists.find(alias->second); old != lists.end()) old->second.aliases.erase(object);
        }
        listAliases[object] = identity.serial; list.aliases.insert(object);
        return &list;
    }
    void Error(Stream& s, const std::string& message) {
        if (s.error != message) { s.error = message; status("stream " + std::to_string(s.id) + ": " + message); }
    }
    bool Selected(const Stream& s) const { return s.id == (config.streamId ? config.streamId : defaultStream); }
    bool GpuOutput() const { return (config.reserved & CaptureGpuOutput) != 0; }
    bool Released(uint64_t sequence) const { return gpuReleased && gpuReleased(sequence); }
    bool EmitGpu(const Candidate& c, uint32_t width, uint32_t height, DXGI_FORMAT format,
                 const SharedSurface& shared, Stream& s) {
        if (c.token <= lastPublished || !Selected(s) || !enabled || !GpuOutput() || !gpuFrame) return false;
        GpuFrame f; f.width = width; f.height = height; f.format = LinearFormat(format);
        f.sequence = c.token; f.timestamp100ns = c.timestamp; f.adapterLuid = shared.adapter;
        wcsncpy_s(f.textureName, shared.name.c_str(), _TRUNCATE);
        if (!gpuFrame(f)) return false;
        lastPublished = c.token; s.lastSendTime = c.timestamp;
        Error(s, "capturing " + std::to_string(width) + "x" + std::to_string(height) + " GPU SBS");
        return true;
    }
    void Emit(const Frame& f, Stream& s) {
        if (f.sequence <= lastPublished || !Selected(s) || !enabled) return;
        lastPublished = f.sequence; s.lastSendTime = f.timestamp100ns;
        frame(f); Error(s, "capturing " + std::to_string(f.width) + "x" + std::to_string(f.height) + " BGRA8 SBS");
    }
    void Poll11(Stream& s) {
        for (auto& slot : s.slots11) {
            if (!slot.busy || !slot.decided) continue;
            if (slot.sent) { if (Released(slot.token)) slot.busy = false; continue; }
            const HRESULT ready = slot.context->GetData(slot.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (ready == S_FALSE) continue;
            if (FAILED(ready)) { slot.busy = false; Error(s, "DX11 query failed; frame dropped"); continue; }
            if (slot.selected && enabled) {
                if (slot.shared) {
                    slot.sent = EmitGpu(slot, slot.width, slot.height, slot.format, *slot.shared, s);
                    slot.busy = slot.sent; continue;
                }
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const auto hr = slot.context->Map(slot.readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
                if (SUCCEEDED(hr)) {
                    auto f = ReadPixels(mapped.pData, mapped.RowPitch, slot.width, slot.height, slot.format, slot.token, slot.timestamp);
                    slot.context->Unmap(slot.readback.Get(), 0); Emit(f, s);
                } else Error(s, "DX11 staging Map failed");
            }
            slot.busy = false;
        }
        // Do not access a different device's immediate context on this thread.
        std::erase_if(retired11, [&](Slot11& slot) {
            if (slot.sent) return Released(slot.token);
            if (slot.context.Get() != s.context11.Get()) return false;
            return slot.context->GetData(slot.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_FALSE;
        });
    }
    void Capture11(Stream& s, const Candidate& c) {
        if (!s.context11 || s.context11->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            Error(s, "DX11 requires the observed immediate context; deferred contexts are unsupported"); return;
        }
        Poll11(s);
        ComPtr<ID3D11Resource> resource;
        DXGI_FORMAT format{}; UINT mip{}, slice{};
        if (s.view11) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{}; s.view11->GetDesc(&v); format = v.Format;
            if (v.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) mip = v.Texture2D.MostDetailedMip;
            else if (v.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY && v.Texture2DArray.ArraySize == 1) {
                mip = v.Texture2DArray.MostDetailedMip; slice = v.Texture2DArray.FirstArraySlice;
            } else { Error(s, "DX11 requires a single 2D SRV slice; MSAA/arrays/cubes are unsupported"); return; }
            s.view11->GetResource(&resource);
        } else if (s.target11) {
            D3D11_RENDER_TARGET_VIEW_DESC v{}; s.target11->GetDesc(&v); format = v.Format;
            if (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = v.Texture2D.MipSlice;
            else if (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && v.Texture2DArray.ArraySize == 1) {
                mip = v.Texture2DArray.MipSlice; slice = v.Texture2DArray.FirstArraySlice;
            } else { Error(s, "DX11 internal buffer view is not a single 2D slice"); return; }
            s.target11->GetResource(&resource);
        } else { Error(s, "waiting for DX11 setInputViewTexture/setInputFrameBuffer/getFrameBuffer"); return; }
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) { Error(s, "DX11 input is not Texture2D"); return; }
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        const auto w = (std::max)(1u, d.Width >> mip), h = (std::max)(1u, d.Height >> mip);
        if (!Supported(format) || !Dimensions(w, h) || d.SampleDesc.Count != 1) {
            Error(s, "unsupported DX11 input: require even SBS width, SDR RGBA8/BGRA8, non-MSAA"); return;
        }
        auto it = std::find_if(s.slots11.begin(), s.slots11.end(), [](const auto& x) { return !x.busy; });
        if (it == s.slots11.end()) { Error(s, "DX11 capture ring full; latest weave dropped"); return; }
        auto& slot = *it;
        ComPtr<ID3D11Device> device, contextDevice; texture->GetDevice(&device); s.context11->GetDevice(&contextDevice);
        if (device.Get() != contextDevice.Get()) { Error(s, "DX11 input/context device mismatch"); return; }
        // Reusing an idle slot on a different device requires new query/resources.
        ComPtr<ID3D11Device> oldDevice; if (slot.readback) slot.readback->GetDevice(&oldDevice);
        if (!slot.readback || slot.width != w || slot.height != h || slot.format != format || oldDevice.Get() != device.Get() || bool(slot.shared) != GpuOutput()) {
            slot.readback.Reset(); slot.query.Reset(); slot.shared.reset();
            uint64_t heldBytes{}; size_t heldSlots{};
            const auto count = [&](const Slot11& x) {
                if (x.readback) { ++heldSlots; heldBytes += uint64_t(x.width) * x.height * 4; }
            };
            for (auto& [_, stream] : streams) for (auto& x : stream.slots11) {
                // A changed selection must not retain an idle old device's
                // cache forever. Busy or externally leased slots stay pinned.
                if (!Selected(stream) && !x.busy) { x.readback.Reset(); x.query.Reset(); x.shared.reset(); }
                count(x);
            }
            for (const auto& x : retired11) count(x);
            if (heldSlots >= 24 || heldBytes + uint64_t(w) * h * 4 > 512ull * 1024 * 1024) {
                Error(s, "DX11 capture allocations at limit; waiting for GPU completion and consumer release"); return;
            }
            D3D11_TEXTURE2D_DESC out{}; out.Width = w; out.Height = h; out.MipLevels = out.ArraySize = out.SampleDesc.Count = 1;
            out.Format = format; out.Usage = D3D11_USAGE_STAGING; out.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (GpuOutput()) {
                out.Format = LinearFormat(format); out.Usage = D3D11_USAGE_DEFAULT; out.CPUAccessFlags = 0;
                out.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                // Ownership is enforced by completed-query publication plus the
                // consumer lease. Never wait for a consumer on the game thread.
                out.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            }
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            if (FAILED(device->CreateTexture2D(&out, nullptr, &slot.readback)) || FAILED(device->CreateQuery(&query, &slot.query))) {
                slot.readback.Reset(); Error(s, "DX11 capture allocation failed"); return;
            }
            if (GpuOutput()) {
                auto shared = std::make_shared<SharedSurface>(); ComPtr<IDXGIResource1> sharedResource;
                ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC adapterDesc{};
                if (FAILED(slot.readback.As(&sharedResource)) || FAILED(device.As(&dxgiDevice)) ||
                    FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&adapterDesc)) ||
                    FAILED(sharedResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        shared->name.c_str(), &shared->handle))) {
                    slot.readback.Reset(); Error(s, "DX11 shared NT texture allocation failed"); return;
                }
                shared->adapter = adapterDesc.AdapterLuid; slot.shared = std::move(shared);
            }
        }
        static_cast<Candidate&>(slot) = c; slot.sent = false; slot.width = w; slot.height = h; slot.format = format; slot.context = s.context11;
        s.context11->CopySubresourceRegion(slot.readback.Get(), 0, 0, 0, 0, texture.Get(), D3D11CalcSubresource(mip, slice, d.MipLevels), nullptr);
        s.context11->End(slot.query.Get()); slot.busy = true;
    }
    void Capture12(Stream& s, const Candidate& c) {
        if (!s.input12 || !s.list12 || s.list12->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) { Error(s, "waiting for DX12 input and direct command list"); return; }
        auto found = FindList(s.list12);
        if (!found) { Error(s, "DX12 command-list PrivateData identity is unavailable or changed; no copy recorded"); return; }
        auto& list = *found; const auto state = list.state.find(s.input12.Get());
        if (state == list.state.end()) { Error(s, "DX12 input state unknown in this command recording; skipping safely"); return; }
        if (!list.submissionObserved) {
            if (s.identityStatusCount == UINT64_MAX || submittedLists - s.identityStatusCount >= 120) {
                std::ostringstream message; message << "DX12 submission identity pending for list " << s.list12 << "; latest native list " << lastSubmittedList
                    << ", observed " << submittedLists << ", unmatched " << unmatchedLists << "; no copy recorded";
                Error(s, message.str()); s.identityStatusCount = submittedLists;
            }
            return;
        }
        const auto d = s.input12->GetDesc(); auto format = s.format12 == DXGI_FORMAT_UNKNOWN ? d.Format : s.format12;
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
            d.SampleDesc.Count != 1 || !Supported(format) || !Dimensions(d.Width, d.Height)) {
            Error(s, "unsupported DX12 input: require SDR RGBA8/BGRA8, 2D, one mip/slice, non-MSAA"); return;
        }
        ComPtr<ID3D12Device> device, listDevice;
        if (FAILED(s.input12->GetDevice(IID_PPV_ARGS(&device))) || FAILED(s.list12->GetDevice(IID_PPV_ARGS(&listDevice))) || device.Get() != listDevice.Get()) {
            Error(s, "DX12 input/list device mismatch"); return;
        }
        std::shared_ptr<Slot12> slot;
        if (GpuOutput()) {
            const auto reusable = std::find_if(reusable12.begin(), reusable12.end(), [&](const auto& x) {
                ComPtr<ID3D12Device> oldDevice; x->readback->GetDevice(IID_PPV_ARGS(&oldDevice));
                return x->width == d.Width && x->height == d.Height && x->format == format && oldDevice.Get() == device.Get();
            });
            if (reusable != reusable12.end()) { slot = std::move(*reusable); reusable12.erase(reusable); }
            // Pool allocations not matching the active source need no lifetime
            // quarantine; their GPU work and consumer leases already finished.
            else reusable12.clear();
        }
        uint64_t heldBytes{}; size_t heldSlots = retired.size() + reusable12.size();
        for (const auto& x : retired) heldBytes += x->bytes;
        for (const auto& x : reusable12) heldBytes += x->bytes;
        for (const auto& [_, l] : lists) { heldSlots += l.slots.size(); for (const auto& x : l.slots) heldBytes += x->bytes; }
        if (heldSlots >= 24 || heldBytes + d.Width * d.Height * 4 > 512ull * 1024 * 1024) {
            if (slot) reusable12.push_back(std::move(slot));
            Error(s, "DX12 recorded captures at limit; waiting for Reset, GPU completion and consumer release"); return;
        }
        if (slot && slot->bytes > 512ull * 1024 * 1024 - heldBytes) {
            reusable12.push_back(std::move(slot)); Error(s, "DX12 capture allocations at byte limit"); return;
        }
        if (!slot) slot = std::make_shared<Slot12>();
        static_cast<Candidate&>(*slot) = c; slot->sent = slot->submitted = slot->failedSignal = false; slot->completions.clear();
        slot->width = static_cast<uint32_t>(d.Width); slot->height = d.Height; slot->format = format; slot->streamId = s.id; slot->source = s.input12;
        if (!slot->readback) {
            device->GetCopyableFootprints(&d, 0, 1, 0, &slot->footprint, nullptr, nullptr, &slot->bytes);
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = slot->bytes;
            rd.Height = 1; rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (GpuOutput()) {
                heap.Type = D3D12_HEAP_TYPE_DEFAULT; rd = d; rd.Alignment = 0; rd.Format = LinearFormat(format);
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                slot->bytes = device->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
            }
            if (slot->bytes > 512ull * 1024 * 1024 - heldBytes || FAILED(device->CreateCommittedResource(&heap,
                    GpuOutput() ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &rd,
                    GpuOutput() ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&slot->readback)))) {
                Error(s, "DX12 capture allocation failed"); return;
            }
            if (GpuOutput()) {
                auto shared = std::make_shared<SharedSurface>(); shared->adapter = device->GetAdapterLuid();
                if (FAILED(device->CreateSharedHandle(slot->readback.Get(), nullptr, GENERIC_ALL, shared->name.c_str(), &shared->handle))) {
                    Error(s, "DX12 shared NT texture allocation failed"); return;
                }
                slot->shared = std::move(shared);
            }
        }
        const auto before = state->second;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {s.input12.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) s.list12->ResourceBarrier(1, &barrier);
        D3D12_RESOURCE_BARRIER targetBarrier{}; targetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        targetBarrier.Transition = {slot->readback.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        if (slot->shared) s.list12->ResourceBarrier(1, &targetBarrier);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = s.input12.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = slot->readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = slot->footprint;
        if (slot->shared) { dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0; }
        s.list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(targetBarrier.Transition.StateBefore, targetBarrier.Transition.StateAfter);
        if (slot->shared) s.list12->ResourceBarrier(1, &targetBarrier);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) s.list12->ResourceBarrier(1, &barrier);
        list.slots.push_back(std::move(slot));
    }
    Stream* FindStream(uint32_t id) { for (auto& [_, s] : streams) if (s.id == id) return &s; return nullptr; }
    void Poll12() {
        auto poll = [&](const std::shared_ptr<Slot12>& slot, bool recordingRetired) {
            if (!slot->decided || !slot->selected || slot->published || !slot->Ready()) return;
            // A closed command list may be executed again. Only Reset or its
            // final destruction proves no future writes can race the consumer.
            if (slot->shared && !recordingRetired) return;
            slot->published = true;
            auto s = FindStream(slot->streamId); if (!s || !enabled) return;
            if (slot->shared) {
                slot->sent = EmitGpu(*slot, slot->width, slot->height, slot->format, *slot->shared, *s); return;
            }
            void* data{}; const D3D12_RANGE range{0, static_cast<SIZE_T>(slot->bytes)};
            if (SUCCEEDED(slot->readback->Map(0, &range, &data))) {
                auto f = ReadPixels(static_cast<uint8_t*>(data) + slot->footprint.Offset, slot->footprint.Footprint.RowPitch,
                    slot->width, slot->height, slot->format, slot->token, slot->timestamp);
                const D3D12_RANGE written{0, 0}; slot->readback->Unmap(0, &written); Emit(f, *s);
            } else Error(*s, "DX12 readback Map failed");
        };
        for (auto& [_, list] : lists) for (const auto& slot : list.slots) poll(slot, false);
        for (const auto& slot : retired) poll(slot, true);
        // A recorded readback stays alive until Reset, even after its fence has
        // completed: an application may execute that same command list again.
        // An unobserved submission is not proof that GPU work never happened.
        // Keep those readbacks quarantined under the global slot/byte cap.
        std::erase_if(retired, [&](const auto& x) {
            if (!x->submitted || !x->decided || !x->Ready() || (x->sent && !Released(x->token))) return false;
            if (x->shared && reusable12.size() < 6) {
                x->source.Reset(); x->completions.clear(); reusable12.push_back(x);
            }
            return true;
        });
    }
};

Core::Core(FrameSink f, StatusSink s) : p_(std::make_unique<Impl>(std::move(f), std::move(s))) {}
Core::~Core() = default;
void Core::Configure(const CaptureConfig& c) { std::lock_guard lock(p_->mutex); p_->config = c; }
void Core::SetGpuOutput(GpuFrameSink publish, GpuFrameReleased released) {
    std::lock_guard lock(p_->mutex); p_->gpuFrame = std::move(publish); p_->gpuReleased = std::move(released);
}
void Core::SetEnabled(bool enabled) {
    std::lock_guard lock(p_->mutex);
    if (p_->enabled == enabled) return;
    p_->enabled = enabled;
    if (enabled) return;
    // Keep in-flight allocations alive, but never publish a pre-stop copy when
    // the same weaver resumes sharing. GPU recordings can still execute later.
    const auto discard = [](auto& c) { c.decided = true; c.selected = false; };
    for (auto& [_, s] : p_->streams) {
        ++s.epoch; s.ordinal = 0; s.lastToken = s.chosenToken = s.lastSendTime = s.nextCaptureTime = 0;
        s.captureEpoch = false;
        for (auto& slot : s.slots11) if (slot.busy) discard(slot);
    }
    for (auto& [_, list] : p_->lists) for (auto& slot : list.slots) discard(*slot);
    for (auto& slot : p_->retired) discard(*slot);
}
uint32_t Core::Register(void* key, HWND window, bool dx12) {
    std::lock_guard lock(p_->mutex); auto [it, added] = p_->streams.try_emplace(key); auto& s = it->second;
    if (added) { s.id = p_->nextStream++; s.window = window; s.dx12 = dx12; if (!p_->defaultStream) p_->defaultStream = s.id;
        std::ostringstream text; text << "available stream " << s.id << " " << (dx12 ? "DX12" : "DX11") << " HWND=0x" << std::hex << reinterpret_cast<uintptr_t>(window);
        p_->status(text.str()); }
    return s.id;
}
void Core::Remove(void* key) {
    std::lock_guard lock(p_->mutex); const auto it = p_->streams.find(key); if (it == p_->streams.end()) return;
    const bool wasDefault = it->second.id == p_->defaultStream;
    const auto streamId = it->second.id;
    for (auto& slot : it->second.slots11) if (slot.busy) p_->retired11.push_back(std::move(slot));
    auto discard = [&](auto& slot) { if (slot->streamId == streamId) { slot->decided = true; slot->selected = false; } };
    for (auto& [_, list] : p_->lists) for (auto& slot : list.slots) discard(slot);
    for (auto& slot : p_->retired) discard(slot);
    p_->streams.erase(it);
    if (wasDefault) p_->defaultStream = p_->streams.empty() ? 0 : p_->streams.begin()->second.id;
    p_->Poll12();
}
void Core::Window(void* key, HWND w) { std::lock_guard lock(p_->mutex); if (p_->streams.contains(key)) p_->streams[key].window = w; }
bool Core::HasInput(void* key) {
    std::lock_guard lock(p_->mutex); const auto it = p_->streams.find(key); if (it == p_->streams.end()) return false;
    const auto& s = it->second; return s.dx12 ? s.input12 != nullptr : s.view11 != nullptr || s.target11 != nullptr;
}
void Core::Input11(void* key, ID3D11ShaderResourceView* v) { std::lock_guard lock(p_->mutex); if (p_->streams.contains(key)) { p_->streams[key].view11 = v; p_->streams[key].target11.Reset(); } }
void Core::Input11Target(void* key, ID3D11RenderTargetView* v) { std::lock_guard lock(p_->mutex); if (p_->streams.contains(key) && !p_->streams[key].view11) p_->streams[key].target11 = v; }
void Core::Context11(void* key, ID3D11DeviceContext* c, bool onlyIfUnset) {
    std::lock_guard lock(p_->mutex); const auto it = p_->streams.find(key); if (it == p_->streams.end()) return;
    auto& s = it->second;
    // A public setContext call is authoritative, including null/deferred
    // contexts. The hot-attach fallback must never silently replace it.
    if (!onlyIfUnset || (!s.context11Observed && !s.context11)) s.context11 = c;
    if (!onlyIfUnset) s.context11Observed = true;
}
void Core::Input12(void* key, ID3D12Resource* r, DXGI_FORMAT f, bool onlyIfUnset) {
    std::lock_guard lock(p_->mutex);
    if (p_->streams.contains(key) && (!onlyIfUnset || !p_->streams[key].input12)) { p_->streams[key].input12 = r; p_->streams[key].format12 = f; }
}
void Core::CommandList12(void* key, ID3D12GraphicsCommandList* l) {
    std::lock_guard lock(p_->mutex);
    if (p_->streams.contains(key)) { p_->RegisterList(l); p_->streams[key].list12 = l; }
}
void Core::BeforeWeave(void* key) {
    std::lock_guard lock(p_->mutex); const auto it = p_->streams.find(key); if (it == p_->streams.end()) return; auto& s = it->second;
    ++s.ordinal; s.lastToken = ++p_->token; if (s.ordinal == p_->config.weaveOrdinal) s.chosenToken = s.lastToken;
    if (!p_->enabled || !p_->Selected(s)) return;
    if (p_->GpuOutput() && (!p_->gpuFrame || !p_->gpuReleased)) { p_->Error(s, "GPU frame output is unavailable"); return; }
    const auto now = Timestamp100ns();
    // Select whole Present epochs before allocating or issuing copies. All
    // weaves in an eligible epoch remain candidates, preserving final-weave
    // selection for runtimes which first weave a mask and then the real image.
    if (s.ordinal == 1) {
        s.captureEpoch = !p_->config.maxFps || now >= s.nextCaptureTime;
        if (s.captureEpoch && p_->config.maxFps) {
            const auto period = (std::max)(1ull, 10000000ull / p_->config.maxFps);
            if (!s.nextCaptureTime || now - s.nextCaptureTime > period * 4) s.nextCaptureTime = now + period;
            else do { s.nextCaptureTime += period; } while (s.nextCaptureTime <= now);
        }
    }
    if (!s.captureEpoch) return;
    if (p_->config.weaveOrdinal && p_->config.weaveOrdinal != s.ordinal) return;
    Impl::Candidate c{}; c.token = s.lastToken; c.timestamp = now; c.epoch = s.epoch; c.ordinal = s.ordinal;
    if (s.dx12) p_->Capture12(s, c); else p_->Capture11(s, c);
}
void Core::Present(HWND w) {
    std::lock_guard lock(p_->mutex);
    for (auto& [_, s] : p_->streams) {
        const auto target = p_->config.window ? reinterpret_cast<HWND>(p_->config.window) : s.window;
        if (target && target != w) continue;
        const auto chosen = p_->config.weaveOrdinal ? s.chosenToken : s.lastToken;
        auto decide = [&](Impl::Candidate& c) { if (c.epoch == s.epoch) { c.decided = true; c.selected = c.token == chosen; } };
        if (s.dx12) {
            for (auto& [__, l] : p_->lists) for (auto& slot : l.slots) if (slot->streamId == s.id) decide(*slot);
            for (auto& slot : p_->retired) if (slot->streamId == s.id) decide(*slot);
        } else { for (auto& slot : s.slots11) if (slot.busy) decide(slot); p_->Poll11(s); }
        ++s.epoch; s.ordinal = 0; s.lastToken = s.chosenToken = 0;
    }
    p_->Poll12();
}
void Core::Poll12() { std::lock_guard lock(p_->mutex); p_->Poll12(); }
Diagnostics12 Core::GetDiagnostics12() {
    std::lock_guard lock(p_->mutex); Diagnostics12 result;
    result.trackedLists = p_->lists.size(); result.retiredSlots = p_->retired.size();
    result.submittedLists = p_->submittedLists; result.unmatchedLists = p_->unmatchedLists;
    for (const auto& [_, list] : p_->lists) { result.confirmedLists += list.submissionObserved; result.recordingSlots += list.slots.size(); }
    return result;
}
bool Core::OwnsList12(ID3D12CommandList* list) {
    std::lock_guard lock(p_->mutex); return p_->FindList(static_cast<ID3D12GraphicsCommandList*>(list)) != nullptr;
}
void Core::Barrier12(ID3D12GraphicsCommandList* l, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    std::lock_guard lock(p_->mutex); auto found = p_->FindList(l); if (!found) return; auto& list = *found;
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) { list.state.clear(); continue; }
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const auto& t = b.Transition;
        if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE || (t.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && t.Subresource != 0)) list.state.erase(t.pResource);
        else list.state[t.pResource] = t.StateAfter;
    }
}
void Core::Invalidate12(ID3D12GraphicsCommandList* l) { std::lock_guard lock(p_->mutex); if (auto list = p_->FindList(l)) list->state.clear(); }
void Core::Reset12(ID3D12GraphicsCommandList* l) {
    std::lock_guard lock(p_->mutex); auto found = p_->FindList(l); if (!found) return; auto& list = *found;
    p_->retired.insert(p_->retired.end(), list.slots.begin(), list.slots.end()); list.slots.clear(); list.state.clear(); p_->Poll12();
}
void Core::ForgetList12(ID3D12GraphicsCommandList* l) {
    std::lock_guard lock(p_->mutex);
    // Release has already reached zero: never call a method on l here.
    const auto alias = p_->listAliases.find(l); if (alias == p_->listAliases.end()) return;
    const auto found = p_->lists.find(alias->second); p_->listAliases.erase(alias);
    if (found == p_->lists.end()) return;
    auto& list = found->second; list.aliases.erase(l);
    // A wrapper may die while its native recording is still re-executable.
    // Keep ownership until all observed interfaces die or a real Reset occurs.
    if (!list.aliases.empty()) return;
    p_->retired.insert(p_->retired.end(), list.slots.begin(), list.slots.end()); p_->lists.erase(found); p_->Poll12();
}
void Core::ForgetQueue12(ID3D12CommandQueue* q) { std::lock_guard lock(p_->mutex); p_->queues.erase(q); }
void Core::Execute12(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists, const std::function<void()>& original) {
    std::lock_guard lock(p_->mutex);
    std::vector<std::shared_ptr<Impl::Slot12>> submitted;
    std::vector<uint64_t> observed;
    for (UINT i = 0; i < count; ++i) {
        ++p_->submittedLists; p_->lastSubmittedList = lists[i];
        auto list = p_->FindList(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (!list) { ++p_->unmatchedLists; continue; }
        observed.push_back(list->identity.serial);
        submitted.insert(submitted.end(), list->slots.begin(), list->slots.end());
    }
    // Serialize submission and CPU mapping, including re-executions and queues.
    original();
    for (const auto identity : observed) if (const auto list = p_->lists.find(identity); list != p_->lists.end()) list->second.submissionObserved = true;
    if (submitted.empty()) return;
    auto& queue = p_->queues[q]; if (!queue.fence) { ComPtr<ID3D12Device> d; q->GetDevice(IID_PPV_ARGS(&d)); if (d) d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.fence)); }
    const auto value = ++queue.value;
    const bool signaled = queue.fence && SUCCEEDED(q->Signal(queue.fence.Get(), value));
    for (auto& slot : submitted) {
        slot->submitted = true;
        if (!signaled) { slot->failedSignal = true; continue; }
        auto it = std::find_if(slot->completions.begin(), slot->completions.end(), [&](const auto& c) { return c.fence.Get() == queue.fence.Get(); });
        if (it == slot->completions.end()) slot->completions.push_back({queue.fence, value}); else it->value = value;
    }
}
}
