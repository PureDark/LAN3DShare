#define LAN3D_CAPTURE_EXPORTS
#include "CaptureCore.h"
#include "HookUtil.h"
#include "GpuSlots.h"
#include "NativeGpuHooks.h"
#include "ModernDiscovery.h"
#include "SetterAnalysis.h"
#include "common/FrameBus.h"
#include "common/GpuFrameBus.h"
#include <sr/weaver/dx11weaver.h>
#include <sr/weaver/dx12weaver.h>
#include <dxgi1_4.h>
#include <Psapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The SDK declares these pure interface destructors without inline definitions.
// Our local probe uses the public headers; it never destroys a runtime object.
namespace SR { IDX11Weaver1::~IDX11Weaver1() = default; IDX12Weaver1::~IDX12Weaver1() = default; }

namespace lan3d::capture {
using Microsoft::WRL::ComPtr;
namespace {
Core* core{};
FrameBus* bus{};
GpuFrameBus* gpuBus{};
std::atomic<bool> active{};
std::recursive_mutex controlMutex;
CaptureConfig config;
std::map<void*, void*> aliases;
std::map<void*, bool> weaverBackends;
std::set<HMODULE> modules;
NativeGpuHooks nativeHooks;
bool nativeHooksConfigured{}, nativePresentReady{}, nativeQueueReady{};
std::array<bool, 96> nativeSitesInstalled{};
std::array<HMODULE, 24> nativeModules{};
std::string nativeStatus, blockedCaptureStatus;
HANDLE worker{};
HANDLE workerWake{};
bool initialized{};
DWORD initializationError{ERROR_DLL_INIT_FAILED};
thread_local unsigned weaveDepth{};
thread_local bool scanning{};
void Status(const std::string& s) { if (bus) bus->SetStatus(s); OutputDebugStringA(("LAN3D: " + s + "\n").c_str()); }
template<class F> void Safe(F&& f) noexcept { try { f(); } catch (...) { Status("capture operation failed; host rendering continues"); } }
void* Key(void* alias) { std::lock_guard lock(controlMutex); const auto it = aliases.find(alias); return it == aliases.end() ? nullptr : it->second; }
void Bind(void* alias, void* key) { std::lock_guard lock(controlMutex); aliases[alias] = key; }
void RegisterWeaver(void* key, HWND window, bool dx12) {
    std::lock_guard lock(controlMutex); weaverBackends[key] = dx12; core->Register(key, window, dx12);
}
void ForgetModernWeaver(void* key);
void Remove(void* alias) {
    std::lock_guard lock(controlMutex); if (auto key = Key(alias)) { ForgetModernWeaver(key); core->Remove(key); weaverBackends.erase(key); std::erase_if(aliases, [&](const auto& x) { return x.second == key; }); }
}
void Capture(void* alias) {
    if (!active || weaveDepth) return;
    void* key{};
    {
        std::lock_guard lock(controlMutex);
        key = Key(alias); if (!key) return;
        const auto backend = weaverBackends.find(key);
        // Never append a GPU copy until both presentation and (for DX12)
        // submission/lifetime observation have verified native entry points.
        if (backend == weaverBackends.end() || !nativePresentReady || (backend->second && !nativeQueueReady)) {
            if (blockedCaptureStatus != nativeStatus) { blockedCaptureStatus = nativeStatus; Status(nativeStatus); }
            return;
        }
        blockedCaptureStatus.clear();
    }
    Safe([&] { core->BeforeWeave(key); });
}
struct WeaveScope { WeaveScope() { ++weaveDepth; } ~WeaveScope() { --weaveDepth; } };
bool AttachList(ID3D12GraphicsCommandList* list);
void AttachQueue(ID3D12CommandQueue* queue);
void AttachDevice(ID3D12Device* device);
void InstallNativeGpuHooks();
void ScanModules();
void InstallModernModule(HMODULE module);
void RecoverModernWeaver(SR::IWeaverBase1* self);
bool ModernInputObserved(void* key);
bool ModernExecutionObserved(void* key);
void AttachLegacy11Device(void* self, ID3D11Device* device, bool inferContext);

// Discover Leia virtual method slots by making calls through the public SDK
// interfaces on an MSVC-generated object. This includes all virtual-base this
// adjustments. The runtime object and its vtables are never replaced.
template<class Interface> class Probe final : public Interface {
public:
    void destroy() override {}
    void setWindowHandle(HWND) override {}
    void weave() override {}
    void enableLateLatching(bool) override {}
    bool isLateLatchingEnabled() const override { return false; }
    void setShaderSRGBConversion(bool, bool) override {}
    void setLatencyInFrames(uint64_t) override {}
    void setLatency(uint64_t) override {}
    uint64_t getLatency() const override { return 0; }
    void getPredictedEyePositions(float[3], float[3]) override {}
    void setInputViewTexture(ID3D11ShaderResourceView*, int, int, DXGI_FORMAT) {}
    void setInputViewTexture(ID3D12Resource*, int, int, DXGI_FORMAT) {}
    void setContext(ID3D11DeviceContext*) {}
    void setOutputFormat(DXGI_FORMAT) {}
    void setCommandList(ID3D12GraphicsCommandList*) {}
    void setViewport(D3D12_VIEWPORT) {}
    void setScissorRect(D3D12_RECT) {}
    ~Probe() override = default;
protected:
    void* queryInterface(std::type_index) override { return nullptr; }
};
thread_local unsigned discoveredSlot;
template<class Signature> struct Sentinel;
template<class R, class... A> struct Sentinel<R(A...)> {
    template<size_t I> static R Call(A...) { discoveredSlot = static_cast<unsigned>(I); if constexpr (!std::is_void_v<R>) return {}; }
    template<size_t... I> static auto Table(std::index_sequence<I...>) { return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&Call<I>)...}; }
};
template<class Signature, class Interface, class Invoke> unsigned Discover(Interface* object, Invoke invoke) {
    auto table = Sentinel<Signature>::Table(std::make_index_sequence<32>{});
    auto& vptr = *reinterpret_cast<void***>(object); auto old = vptr; discoveredSlot = ~0u; vptr = table.data();
    invoke(object); vptr = old; return discoveredSlot;
}
__declspec(noinline) void InvokeWeave(SR::IWeaverBase1* p) { p->weave(); }
__declspec(noinline) void InvokeWindow(SR::IWeaverBase1* p) { p->setWindowHandle(nullptr); }
__declspec(noinline) void InvokeDestroy(SR::IDestroyable* p) { p->destroy(); }
__declspec(noinline) void InvokeInput11(SR::IDX11Weaver1* p) { p->setInputViewTexture(nullptr, 0, 0, DXGI_FORMAT_UNKNOWN); }
__declspec(noinline) void InvokeContext11(SR::IDX11Weaver1* p) { p->setContext(nullptr); }
__declspec(noinline) void InvokeInput12(SR::IDX12Weaver1* p) { p->setInputViewTexture(nullptr, 0, 0, DXGI_FORMAT_UNKNOWN); }
__declspec(noinline) void InvokeList12(SR::IDX12Weaver1* p) { p->setCommandList(nullptr); }
struct LeiaSlots {
    unsigned weave{}, window{}, destroy{}, input11{}, context11{}, input12{}, list12{};
    LeiaSlots() {
        Probe<SR::IDX11Weaver1> a; Probe<SR::IDX12Weaver1> b;
        weave = Discover<void(SR::IWeaverBase1*)>(static_cast<SR::IWeaverBase1*>(&a), InvokeWeave);
        window = Discover<void(SR::IWeaverBase1*, HWND)>(static_cast<SR::IWeaverBase1*>(&a), InvokeWindow);
        destroy = Discover<void(SR::IDestroyable*)>(static_cast<SR::IDestroyable*>(&a), InvokeDestroy);
        input11 = Discover<void(SR::IDX11Weaver1*, ID3D11ShaderResourceView*, int, int, DXGI_FORMAT)>(static_cast<SR::IDX11Weaver1*>(&a), InvokeInput11);
        context11 = Discover<void(SR::IDX11Weaver1*, ID3D11DeviceContext*)>(static_cast<SR::IDX11Weaver1*>(&a), InvokeContext11);
        input12 = Discover<void(SR::IDX12Weaver1*, ID3D12Resource*, int, int, DXGI_FORMAT)>(static_cast<SR::IDX12Weaver1*>(&b), InvokeInput12);
        list12 = Discover<void(SR::IDX12Weaver1*, ID3D12GraphicsCommandList*)>(static_cast<SR::IDX12Weaver1*>(&b), InvokeList12);
    }
};
LeiaSlots* leiaSlots{};

struct WeaverWeave {
    using Signature = void(SR::IWeaverBase1*);
    static void Call(auto original, void*, SR::IWeaverBase1* self) {
        if (active && !weaveDepth) Safe([&] { RecoverModernWeaver(self); });
        Capture(self); WeaveScope scope; original(self);
    }
};
struct WeaverWindow {
    using Signature = void(SR::IWeaverBase1*, HWND);
    static void Call(auto original, void*, SR::IWeaverBase1* self, HWND w) { original(self, w); if (auto key = Key(self)) Safe([&] { core->Window(key, w); }); }
};
struct WeaverDestroy {
    using Signature = void(SR::IDestroyable*);
    static void Call(auto original, void*, SR::IDestroyable* self) { Safe([&] { Remove(self); }); original(self); }
};
struct Input11 {
    using Signature = void(SR::IDX11Weaver1*, ID3D11ShaderResourceView*, int, int, DXGI_FORMAT);
    static void Call(auto original, void*, SR::IDX11Weaver1* self, ID3D11ShaderResourceView* v, int w, int h, DXGI_FORMAT f) {
        original(self, v, w, h, f); if (auto key = Key(self)) Safe([&] {
            const bool existing = ModernInputObserved(key); core->Input11(key, v);
            if (existing && v) { ComPtr<ID3D11Device> device; v->GetDevice(&device); AttachLegacy11Device(key, device.Get(), false); }
        });
    }
};
struct Context11 {
    using Signature = void(SR::IDX11Weaver1*, ID3D11DeviceContext*);
    static void Call(auto original, void*, SR::IDX11Weaver1* self, ID3D11DeviceContext* c) {
        original(self, c); if (auto key = Key(self)) Safe([&] {
            const bool existing = ModernExecutionObserved(key); core->Context11(key, c);
            if (existing && c) { ComPtr<ID3D11Device> device; c->GetDevice(&device); AttachLegacy11Device(key, device.Get(), false); }
        });
    }
};
struct Input12 {
    using Signature = void(SR::IDX12Weaver1*, ID3D12Resource*, int, int, DXGI_FORMAT);
    static void Call(auto original, void*, SR::IDX12Weaver1* self, ID3D12Resource* r, int w, int h, DXGI_FORMAT f) {
        original(self, r, w, h, f); if (auto key = Key(self)) Safe([&] {
            const bool existing = ModernInputObserved(key); core->Input12(key, r, f);
            if (existing && r) { ComPtr<ID3D12Device> device; if (SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&device)))) AttachDevice(device.Get()); }
        });
    }
};
struct CommandList12 {
    using Signature = void(SR::IDX12Weaver1*, ID3D12GraphicsCommandList*);
    static void Call(auto original, void*, SR::IDX12Weaver1* self, ID3D12GraphicsCommandList* list) {
        original(self, list); if (auto key = Key(self)) Safe([&] {
            const bool existing = ModernExecutionObserved(key);
            if (existing && list) { ComPtr<ID3D12Device> device; if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) AttachDevice(device.Get()); }
            core->CommandList12(key, AttachList(list) ? list : nullptr);
        });
    }
};
template<class Interface> void AttachWeaver(Interface* p, HWND window, bool dx12) {
    auto base = static_cast<SR::IWeaverBase1*>(p); auto destroy = static_cast<SR::IDestroyable*>(p);
    RegisterWeaver(base, window, dx12); Bind(p, base); Bind(base, base); Bind(destroy, base);
    if (!Hook<WeaverWeave>(Method(base, leiaSlots->weave)) || !Hook<WeaverWindow>(Method(base, leiaSlots->window)) ||
        !Hook<WeaverDestroy>(Method(destroy, leiaSlots->destroy))) Status("failed to attach a Leia base method; capture may be unavailable for this object");
}
struct Create11 {
    using Signature = WeaverErrorCode(SR::SRContext*, ID3D11DeviceContext*, HWND, SR::IDX11Weaver1**);
    static WeaverErrorCode Call(auto original, void*, SR::SRContext* sr, ID3D11DeviceContext* ctx, HWND w, SR::IDX11Weaver1** out) {
        const auto result = original(sr, ctx, w, out);
        if (result == WeaverSuccess && out && *out) Safe([&] {
            AttachWeaver(*out, w, false); core->Context11(Key(*out), ctx);
            if (!Hook<Input11>(Method(*out, leiaSlots->input11)) || !Hook<Context11>(Method(*out, leiaSlots->context11))) Status("cannot hook DX11 input/context");
        });
        return result;
    }
};
struct Create12 {
    using Signature = WeaverErrorCode(SR::SRContext*, ID3D12Device*, HWND, SR::IDX12Weaver1**);
    static WeaverErrorCode Call(auto original, void*, SR::SRContext* sr, ID3D12Device* d, HWND w, SR::IDX12Weaver1** out) {
        if (d) Safe([&] { AttachDevice(d); });
        const auto result = original(sr, d, w, out);
        if (result == WeaverSuccess && out && *out) Safe([&] {
            AttachWeaver(*out, w, true);
            if (!Hook<Input12>(Method(*out, leiaSlots->input12)) || !Hook<CommandList12>(Method(*out, leiaSlots->list12))) Status("cannot hook DX12 input/command list");
        });
        return result;
    }
};

struct Present {
    using Signature = HRESULT(IDXGISwapChain*, UINT, UINT);
    static HRESULT Call(auto original, void*, IDXGISwapChain* self, UINT interval, UINT flags) {
        if (!(flags & DXGI_PRESENT_TEST)) Safe([&] { DXGI_SWAP_CHAIN_DESC d{}; if (SUCCEEDED(self->GetDesc(&d))) core->Present(d.OutputWindow); });
        return original(self, interval, flags);
    }
};
struct Present1 {
    using Signature = HRESULT(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static HRESULT Call(auto original, void*, IDXGISwapChain1* self, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
        if (!(flags & DXGI_PRESENT_TEST)) Safe([&] { HWND w{}; if (SUCCEEDED(self->GetHwnd(&w))) core->Present(w); });
        return original(self, interval, flags, params);
    }
};
struct Barrier {
    using Signature = void(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    static void Call(auto original, void*, ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b) {
        original(self, n, b); Safe([&] { core->Barrier12(self, n, b); });
    }
};
struct EnhancedBarrier {
    using Signature = void(ID3D12GraphicsCommandList7*, UINT, const D3D12_BARRIER_GROUP*);
    static void Call(auto original, void*, ID3D12GraphicsCommandList7* self, UINT n, const D3D12_BARRIER_GROUP* b) {
        original(self, n, b); Safe([&] { core->Invalidate12(self); });
    }
};
struct ExecuteBundle {
    using Signature = void(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
    static void Call(auto original, void*, ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle) {
        original(self, bundle); Safe([&] { core->Invalidate12(self); });
    }
};
struct Reset {
    using Signature = HRESULT(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    static HRESULT Call(auto original, void*, ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
        const auto result = original(self, a, p); if (SUCCEEDED(result)) Safe([&] { core->Reset12(self); }); return result;
    }
};
struct ListRelease {
    using Signature = ULONG(ID3D12GraphicsCommandList*);
    static ULONG Call(auto original, void*, ID3D12GraphicsCommandList* self) {
        const auto result = original(self); if (!result) Safe([&] { core->ForgetList12(self); core->ForgetQueue12(reinterpret_cast<ID3D12CommandQueue*>(self)); }); return result;
    }
};
struct Execute {
    using Signature = void(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    static void Call(auto original, void*, ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
        // These are objects actually submitted by the host. Observe native
        // Reset/Release as well as a possible outer wrapper's public methods.
        Safe([&] { for (UINT i = 0; i < n; ++i) {
            ComPtr<ID3D12GraphicsCommandList> list;
            if (lists[i] && core->OwnsList12(lists[i]) && SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&list)))) AttachList(list.Get());
        } });
        // Do not retry original if capture processing throws after submission.
        bool submitted{};
        try { core->Execute12(self, n, lists, [&] { submitted = true; original(self, n, lists); }); }
        catch (...) { Status("DX12 submit bookkeeping failed"); if (!submitted) original(self, n, lists); }
    }
};
std::set<void*> attachedListTables;
std::mutex listAttachMutex;
bool AttachList(ID3D12GraphicsCommandList* list) {
    if (!list) return false;
    // Queue wrappers may nest Execute hooks while Core holds its own mutex.
    // Never acquire the global control mutex from that submission path.
    std::lock_guard lock(listAttachMutex);
    auto table = *reinterpret_cast<void***>(list);
    if (attachedListTables.contains(table)) return true;
    bool ok = Hook<Barrier>(Method(list, CaptureGetGpuSlot(SlotListBarrier)));
    ok &= Hook<Reset>(Method(list, CaptureGetGpuSlot(SlotListReset)));
    ok &= Hook<ListRelease>(Method(list, CaptureGetGpuSlot(SlotListRelease)));
    ok &= Hook<ExecuteBundle>(Method(list, CaptureGetGpuSlot(SlotListExecuteBundle)));
    ComPtr<ID3D12GraphicsCommandList7> list7;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7)))) ok &= Hook<EnhancedBarrier>(Method(list7.Get(), CaptureGetGpuSlot(SlotListEnhancedBarrier)));
    if (!ok) Status("could not hook all DX12 command recording methods");
    else attachedListTables.insert(table);
    return ok;
}
void AttachQueue(ID3D12CommandQueue* queue) {
    if (queue && (!Hook<Execute>(Method(queue, CaptureGetGpuSlot(SlotQueueExecute))) ||
        !Hook<ListRelease>(Method(queue, CaptureGetGpuSlot(SlotListRelease))))) Status("could not hook DX12 queue submission/lifetime");
}
struct CreateQueue {
    using Signature = HRESULT(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    static HRESULT Call(auto original, void*, ID3D12Device* d, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out) {
        const auto result = original(d, desc, iid, out);
        if (SUCCEEDED(result) && out && *out) Safe([&] { ComPtr<ID3D12CommandQueue> q; if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&q)))) AttachQueue(q.Get()); });
        return result;
    }
};
void AttachReturnedList(void* object) { ComPtr<ID3D12GraphicsCommandList> list; if (object && SUCCEEDED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&list)))) AttachList(list.Get()); }
struct CreateList {
    using Signature = HRESULT(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    static HRESULT Call(auto original, void*, ID3D12Device* d, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID iid, void** out) {
        const auto result = original(d, mask, type, a, p, iid, out); if (SUCCEEDED(result) && out) Safe([&] { AttachReturnedList(*out); }); return result;
    }
};
struct CreateList1 {
    using Signature = HRESULT(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
    static HRESULT Call(auto original, void*, ID3D12Device4* d, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** out) {
        const auto result = original(d, mask, type, flags, iid, out); if (SUCCEEDED(result) && out) Safe([&] { AttachReturnedList(*out); }); return result;
    }
};
std::set<void*> attachedDevices;
void AttachDevice(ID3D12Device* device) {
    if (!device) return;
    std::lock_guard lock(controlMutex);
    auto table = *reinterpret_cast<void***>(device);
    if (attachedDevices.contains(table)) return;
    // Observe only objects the application creates. A probe queue/swap chain
    // would run through the host's other hooks and can change its active device.
    bool ok = Hook<CreateQueue>(Method(device, CaptureGetGpuSlot(SlotDeviceCreateQueue)));
    ok &= Hook<CreateList>(Method(device, CaptureGetGpuSlot(SlotDeviceCreateList)));
    ComPtr<ID3D12Device4> d4;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d4)))) ok &= Hook<CreateList1>(Method(d4.Get(), CaptureGetGpuSlot(SlotDeviceCreateList1)));
    if (ok) attachedDevices.insert(table);
    else Status("could not observe all DX12 object creation methods; attachment will retry");
}

const wchar_t* ModuleFileName(const wchar_t* path) {
    const wchar_t* name = path;
    for (auto p = path; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
    return name;
}
bool NativeIdentityMatches(const NativeGpuModule& expected, const NativeGpuModule& actual) {
    return !_wcsicmp(expected.path, actual.path) && expected.timestamp == actual.timestamp &&
        expected.imageSize == actual.imageSize && expected.pdbAge == actual.pdbAge &&
        !std::memcmp(&expected.pdbGuid, &actual.pdbGuid, sizeof(GUID));
}
bool NativeExecutable(HMODULE module, uint32_t rva, uint32_t imageSize) {
    if (!rva || rva >= imageSize) return false;
    MEMORY_BASIC_INFORMATION memory{};
    auto address = reinterpret_cast<const uint8_t*>(module) + rva;
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT || memory.AllocationBase != module ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = memory.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
void InstallNativeGpuHooks() {
    std::lock_guard lock(controlMutex);
    if (!nativeHooksConfigured) return;
    bool present{}, execute{}, release{}, badPresent{}, badQueue{};
    std::string problem;
    for (uint32_t mi = 0; mi < nativeHooks.moduleCount; ++mi) {
        const auto& expected = nativeHooks.modules[mi];
        auto module = nativeModules[mi];
        bool mismatch{};
        if (!module) {
            // Query only modules already present. In particular never load the
            // sender's driver/runtime DLL into the capture process.
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, expected.path, &module)) {
                HMODULE sameName{};
                mismatch = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ModuleFileName(expected.path), &sameName) != FALSE;
            } else {
                NativeGpuModule actual{};
                mismatch = !ReadNativeGpuModule(module, actual) || !NativeIdentityMatches(expected, actual);
                HMODULE pinned{};
                if (!mismatch && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module) nativeModules[mi] = module;
                else { mismatch = true; module = nullptr; }
            }
        }
        for (uint32_t si = 0; si < nativeHooks.siteCount; ++si) {
            const auto& site = nativeHooks.sites[si];
            if (site.module != mi) continue;
            const bool isPresent = site.kind == NativeGpuHook::Present || site.kind == NativeGpuHook::Present1;
            if (mismatch) {
                (isPresent ? badPresent : badQueue) = true;
                problem = "loaded module path/identity differs from the external GPU profile";
                continue;
            }
            if (!module) continue; // A suspended source may not have loaded it yet.
            if (!nativeSitesInstalled[si] && NativeExecutable(module, site.rva, expected.imageSize)) {
                auto code = reinterpret_cast<uint8_t*>(module) + site.rva;
                switch (site.kind) {
                case NativeGpuHook::Present: nativeSitesInstalled[si] = Hook<Present>(code); break;
                case NativeGpuHook::Present1: nativeSitesInstalled[si] = Hook<Present1>(code); break;
                case NativeGpuHook::QueueExecute: nativeSitesInstalled[si] = Hook<Execute>(code); break;
                case NativeGpuHook::QueueRelease: nativeSitesInstalled[si] = Hook<ListRelease>(code); break;
                }
            }
            if (!nativeSitesInstalled[si]) {
                (isPresent ? badPresent : badQueue) = true;
                problem = "a verified native entry is not executable or could not be hooked; installation will retry";
                continue;
            }
            if (isPresent) present = true;
            else if (site.kind == NativeGpuHook::QueueExecute) execute = true;
            else if (site.kind == NativeGpuHook::QueueRelease) release = true;
        }
    }
    nativePresentReady = present && !badPresent;
    nativeQueueReady = execute && release && !badQueue;
    std::string status = "native GPU hooks: ";
    if (!nativePresentReady) status += "capture paused until presentation hooks match the loaded runtime";
    else if (!nativeQueueReady) status += "DX11 ready; DX12 capture paused until queue hooks match the loaded runtime";
    else status += "presentation and DX12 submission hooks verified; no host probe objects created";
    if (!problem.empty()) status += "; " + problem;
    if (nativeStatus != status) { nativeStatus = status; Status(status); }
}

constexpr const char* Create11Name = "?CreateDX11Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D11DeviceContext@@PEAUHWND__@@PEAPEAVIDX11Weaver1@1@@Z";
constexpr const char* Create12Name = "?CreateDX12Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D12Device@@PEAUHWND__@@PEAPEAVIDX12Weaver1@1@@Z";
void InstallLegacy(HMODULE module);
void AttachModule(HMODULE module) {
    std::lock_guard lock(controlMutex);
    if (!modules.insert(module).second) return;
    auto c11 = GetProcAddress(module, Create11Name), c12 = GetProcAddress(module, Create12Name);
    if (!c11 && !c12 && !GetProcAddress(module, "?weave@DX11WeaverBase@SR@@QEAAXII@Z") && !GetProcAddress(module, "?weave@DX12WeaverBase@SR@@QEAAXII@Z")) return;
    HMODULE pinned{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned);
    // Recover tables from the image containing the actual implementation. PE
    // export forwarders may place every method in a different DLL.
    InstallModernModule(module);
    for (const auto code : {c11, c12}) if (code) {
        HMODULE implementation{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(code), &implementation) && implementation != module) InstallModernModule(implementation);
    }
    if (c11) { GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(c11), &pinned); Hook<Create11>(reinterpret_cast<void*>(c11)); }
    if (c12) { GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(c12), &pinned); Hook<Create12>(reinterpret_cast<void*>(c12)); }
    InstallLegacy(module);
    Status("SimulatedRealityDirectX hooks installed; observing existing and new weavers");
}
void ScanModules() {
    if (scanning) return;
    scanning = true;
    Safe([&] {
        InstallNativeGpuHooks();
        std::vector<HMODULE> handles(256); DWORD bytes{};
        if (!EnumProcessModules(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &bytes)) return;
        if (bytes > handles.size() * sizeof(HMODULE)) { handles.resize(bytes / sizeof(HMODULE)); if (!EnumProcessModules(GetCurrentProcess(), handles.data(), bytes, &bytes)) return; }
        for (size_t i = 0; i < (std::min)(handles.size(), size_t(bytes / sizeof(HMODULE))); ++i) {
            wchar_t name[MAX_PATH]{}; if (GetModuleBaseNameW(GetCurrentProcess(), handles[i], name, MAX_PATH) && _wcsicmp(name, L"SimulatedRealityDirectX.dll") == 0) AttachModule(handles[i]);
        }
    });
    scanning = false;
}
template<bool Wide, bool Extended> struct LoadLibraryHook;
void RequestModuleScan() noexcept {
    // A nested LoadLibrary can return while its caller's DllMain still owns
    // the loader lock. Never scan modules, install hooks or take controlMutex
    // on this thread: the worker may hold controlMutex while querying modules.
    // This event is created before these hooks and lives as long as the worker.
    if (workerWake) SetEvent(workerWake);
}
template<> struct LoadLibraryHook<true, false> {
    using Signature = HMODULE(LPCWSTR);
    static HMODULE Call(auto original, void*, LPCWSTR path) { auto r = original(path); if (r && active) RequestModuleScan(); return r; }
};
template<> struct LoadLibraryHook<false, false> {
    using Signature = HMODULE(LPCSTR);
    static HMODULE Call(auto original, void*, LPCSTR path) { auto r = original(path); if (r && active) RequestModuleScan(); return r; }
};
template<> struct LoadLibraryHook<true, true> {
    using Signature = HMODULE(LPCWSTR, HANDLE, DWORD);
    static HMODULE Call(auto original, void*, LPCWSTR path, HANDLE h, DWORD flags) { auto r = original(path, h, flags); if (r && active && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))) RequestModuleScan(); return r; }
};
template<> struct LoadLibraryHook<false, true> {
    using Signature = HMODULE(LPCSTR, HANDLE, DWORD);
    static HMODULE Call(auto original, void*, LPCSTR path, HANDLE h, DWORD flags) { auto r = original(path, h, flags); if (r && active && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))) RequestModuleScan(); return r; }
};

// Legacy export adapters are defined below. Their prototypes come directly from
// dx11weaver.h / dx12weaver.h; they do not inspect private pimpl layouts.
void AttachLegacy11Device(void* self, ID3D11Device* device, bool inferContext) {
    if (!device) return;
    if (inferContext) {
        // Legacy DX11 weavers require the device's immediate context. This
        // Public COM provides this fallback without reading the runtime object;
        // an observed setContext call always remains authoritative.
        ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        core->Context11(self, context.Get(), true);
    }
}
void ObserveLegacy11Input(void* self, ID3D11ShaderResourceView* view) {
    RegisterWeaver(self, nullptr, false); Bind(self, self); core->Input11(self, view);
    ComPtr<ID3D11Device> device; if (view) { view->GetDevice(&device); AttachLegacy11Device(self, device.Get(), true); }
}
void ObserveLegacy11Target(void* self, ID3D11RenderTargetView* view) {
    RegisterWeaver(self, nullptr, false); Bind(self, self); core->Input11Target(self, view);
    ComPtr<ID3D11Device> device; if (view) { view->GetDevice(&device); AttachLegacy11Device(self, device.Get(), true); }
}
void ObserveLegacy12Input(void* self, ID3D12Resource* input, DXGI_FORMAT format, bool onlyIfUnset = false) {
    RegisterWeaver(self, nullptr, true); Bind(self, self); core->Input12(self, input, format, onlyIfUnset);
    ComPtr<ID3D12Device> device; if (input && SUCCEEDED(input->GetDevice(IID_PPV_ARGS(&device)))) AttachDevice(device.Get());
}
void ObserveLegacy12List(void* self, ID3D12GraphicsCommandList* list) {
    RegisterWeaver(self, nullptr, true); Bind(self, self);
    ComPtr<ID3D12Device> device; if (list && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) AttachDevice(device.Get());
    core->CommandList12(self, AttachList(list) ? list : nullptr);
}
template<bool Dx12> void RecoverLegacyInput(void* self, void* code) {
    if (!active || weaveDepth) return;
    RegisterWeaver(self, nullptr, Dx12); Bind(self, self);
    if (core->HasInput(self)) return;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(code), &module)) return;
    // Call only the published getter belonging to the actual implementation
    // (including forwarded exports). Never inspect an existing object's pimpl.
    if constexpr (Dx12) {
        using Getter = ID3D12Resource*(*)(void*);
        const auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "?getFrameBuffer@DX12WeaverBase@SR@@QEAAPEAUID3D12Resource@@XZ"));
        if (getter) { auto input = getter(self); ObserveLegacy12Input(self, input, input ? input->GetDesc().Format : DXGI_FORMAT_UNKNOWN, true); }
    } else {
        using Getter = ID3D11RenderTargetView*(*)(void*);
        const auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "?getFrameBuffer@DX11WeaverBase@SR@@QEAAPEAUID3D11RenderTargetView@@XZ"));
        if (getter) ObserveLegacy11Target(self, getter(self));
    }
}
void RegisterLegacy11(void* self, ID3D11DeviceContext* context, HWND window, void* code) {
    RegisterWeaver(self, window, false); Bind(self, self); core->Window(self, window); core->Context11(self, context);
    HMODULE m{}; if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(code), &m)) {
        using Getter = ID3D11RenderTargetView*(*)(void*);
        auto getter = reinterpret_cast<Getter>(GetProcAddress(m, "?getFrameBuffer@DX11WeaverBase@SR@@QEAAPEAUID3D11RenderTargetView@@XZ"));
        if (getter) core->Input11Target(self, getter(self));
    }
    ComPtr<ID3D11Device> d; if (context) { context->GetDevice(&d); AttachLegacy11Device(self, d.Get(), false); }
}
void RegisterLegacy12(void* self, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* input, HWND window, DXGI_FORMAT format) {
    RegisterWeaver(self, window, true); Bind(self, self); core->Window(self, window); core->Input12(self, input, format); AttachDevice(device); AttachQueue(queue);
}
struct Legacy11Input {
    using Signature = void(void*, ID3D11ShaderResourceView*);
    static void Call(auto o, void*, void* s, ID3D11ShaderResourceView* v) { o(s, v); Safe([&] { ObserveLegacy11Input(s, v); }); }
};
struct Legacy11Get {
    using Signature = ID3D11RenderTargetView*(void*);
    static ID3D11RenderTargetView* Call(auto o, void*, void* s) { auto v = o(s); Safe([&] { ObserveLegacy11Target(s, v); }); return v; }
};
struct Legacy11Context {
    using Signature = void(void*, ID3D11DeviceContext*);
    static void Call(auto o, void*, void* s, ID3D11DeviceContext* c) { o(s, c); Safe([&] {
        RegisterWeaver(s, nullptr, false); Bind(s, s); core->Context11(s, c);
        ComPtr<ID3D11Device> device; if (c) { c->GetDevice(&device); AttachLegacy11Device(s, device.Get(), false); }
    }); }
};
template<bool Dx12> struct LegacyWindow {
    using Signature = void(void*, HWND);
    static void Call(auto o, void*, void* s, HWND w) { o(s, w); Safe([&] { RegisterWeaver(s, w, Dx12); Bind(s, s); core->Window(s, w); }); }
};
struct LegacyDestroy {
    using Signature = void(void*);
    static void Call(auto o, void*, void* s) { Safe([&] { Remove(s); }); o(s); }
};
template<bool Dx12, bool Offset> struct LegacyWeave;
template<bool Dx12> struct LegacyWeave<Dx12, false> {
    using Signature = void(void*, unsigned, unsigned);
    static void Call(auto o, void* code, void* s, unsigned w, unsigned h) { Safe([&] { RecoverLegacyInput<Dx12>(s, code); }); Capture(s); WeaveScope depth; o(s, w, h); }
};
template<bool Dx12> struct LegacyWeave<Dx12, true> {
    using Signature = void(void*, unsigned, unsigned, unsigned, unsigned);
    static void Call(auto o, void* code, void* s, unsigned w, unsigned h, unsigned x, unsigned y) { Safe([&] { RecoverLegacyInput<Dx12>(s, code); }); Capture(s); WeaveScope depth; o(s, w, h, x, y); }
};
struct Legacy12Input {
    using Signature = void(void*, ID3D12Resource*);
    static void Call(auto o, void*, void* s, ID3D12Resource* r) { o(s, r); Safe([&] { ObserveLegacy12Input(s, r, DXGI_FORMAT_UNKNOWN); }); }
};
struct Legacy12InputFormat {
    using Signature = void(void*, ID3D12Resource*, DXGI_FORMAT);
    static void Call(auto o, void*, void* s, ID3D12Resource* r, DXGI_FORMAT f) { o(s, r, f); Safe([&] { ObserveLegacy12Input(s, r, f); }); }
};
struct Legacy12Get {
    using Signature = ID3D12Resource*(void*);
    static ID3D12Resource* Call(auto o, void*, void* s) { auto r = o(s); Safe([&] { ObserveLegacy12Input(s, r, r ? r->GetDesc().Format : DXGI_FORMAT_UNKNOWN, true); }); return r; }
};
struct Legacy12List {
    using Signature = void(void*, ID3D12GraphicsCommandList*);
    static void Call(auto o, void*, void* s, ID3D12GraphicsCommandList* l) { o(s, l); Safe([&] { ObserveLegacy12List(s, l); }); }
};
template<bool Offset> struct LegacyWeaveList;
template<> struct LegacyWeaveList<false> {
    using Signature = void(void*, ID3D12GraphicsCommandList*, unsigned, unsigned);
    static void Call(auto o, void* code, void* s, ID3D12GraphicsCommandList* l, unsigned w, unsigned h) { Safe([&] { ObserveLegacy12List(s, l); RecoverLegacyInput<true>(s, code); }); Capture(s); WeaveScope depth; o(s, l, w, h); }
};
template<> struct LegacyWeaveList<true> {
    using Signature = void(void*, ID3D12GraphicsCommandList*, unsigned, unsigned, unsigned, unsigned);
    static void Call(auto o, void* code, void* s, ID3D12GraphicsCommandList* l, unsigned w, unsigned h, unsigned x, unsigned y) { Safe([&] { ObserveLegacy12List(s, l); RecoverLegacyInput<true>(s, code); }); Capture(s); WeaveScope depth; o(s, l, w, h, x, y); }
};
template<bool Format, bool Window> struct Ctor11;
template<> struct Ctor11<false, false> {
    using Signature = void*(void*, SR::SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned);
    static void* Call(auto o, void* code, void* s, SR::SRContext& sr, ID3D11Device* d, ID3D11DeviceContext* c, unsigned w, unsigned h) { auto r = o(s, sr, d, c, w, h); Safe([&] { RegisterLegacy11(s, c, nullptr, code); }); return r; }
};
template<> struct Ctor11<false, true> {
    using Signature = void*(void*, SR::SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned, HWND);
    static void* Call(auto o, void* code, void* s, SR::SRContext& sr, ID3D11Device* d, ID3D11DeviceContext* c, unsigned w, unsigned h, HWND hwnd) { auto r = o(s, sr, d, c, w, h, hwnd); Safe([&] { RegisterLegacy11(s, c, hwnd, code); }); return r; }
};
template<> struct Ctor11<true, false> {
    using Signature = void*(void*, SR::SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned, DXGI_FORMAT);
    static void* Call(auto o, void* code, void* s, SR::SRContext& sr, ID3D11Device* d, ID3D11DeviceContext* c, unsigned w, unsigned h, DXGI_FORMAT f) { auto r = o(s, sr, d, c, w, h, f); Safe([&] { RegisterLegacy11(s, c, nullptr, code); }); return r; }
};
template<> struct Ctor11<true, true> {
    using Signature = void*(void*, SR::SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned, DXGI_FORMAT, HWND);
    static void* Call(auto o, void* code, void* s, SR::SRContext& sr, ID3D11Device* d, ID3D11DeviceContext* c, unsigned w, unsigned h, DXGI_FORMAT f, HWND hwnd) { auto r = o(s, sr, d, c, w, h, f, hwnd); Safe([&] { RegisterLegacy11(s, c, hwnd, code); }); return r; }
};
template<int Tail> struct Ctor12;
template<> struct Ctor12<0> {
    using Signature = void*(void*, SR::SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource*, ID3D12Resource*);
    static void* Call(auto o, void*, void* s, SR::SRContext& sr, ID3D12Device* d, ID3D12CommandAllocator* a, ID3D12CommandQueue* q, ID3D12Resource* i, ID3D12Resource* out) { Safe([&] { AttachDevice(d); AttachQueue(q); }); auto r = o(s, sr, d, a, q, i, out); Safe([&] { RegisterLegacy12(s, d, q, i, nullptr, DXGI_FORMAT_UNKNOWN); }); return r; }
};
template<> struct Ctor12<1> {
    using Signature = void*(void*, SR::SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource*, ID3D12Resource*, HWND);
    static void* Call(auto o, void*, void* s, SR::SRContext& sr, ID3D12Device* d, ID3D12CommandAllocator* a, ID3D12CommandQueue* q, ID3D12Resource* i, ID3D12Resource* out, HWND hwnd) { Safe([&] { AttachDevice(d); AttachQueue(q); }); auto r = o(s, sr, d, a, q, i, out, hwnd); Safe([&] { RegisterLegacy12(s, d, q, i, hwnd, DXGI_FORMAT_UNKNOWN); }); return r; }
};
template<> struct Ctor12<2> {
    using Signature = void*(void*, SR::SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource*, ID3D12Resource*, HWND, DXGI_FORMAT);
    static void* Call(auto o, void*, void* s, SR::SRContext& sr, ID3D12Device* d, ID3D12CommandAllocator* a, ID3D12CommandQueue* q, ID3D12Resource* i, ID3D12Resource* out, HWND hwnd, DXGI_FORMAT f) { Safe([&] { AttachDevice(d); AttachQueue(q); }); auto r = o(s, sr, d, a, q, i, out, hwnd, f); Safe([&] { RegisterLegacy12(s, d, q, i, hwnd, f); }); return r; }
};
template<> struct Ctor12<3> {
    using Signature = void*(void*, SR::SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource*, ID3D12Resource*, HWND, DXGI_FORMAT, DXGI_FORMAT);
    static void* Call(auto o, void*, void* s, SR::SRContext& sr, ID3D12Device* d, ID3D12CommandAllocator* a, ID3D12CommandQueue* q, ID3D12Resource* i, ID3D12Resource* out, HWND hwnd, DXGI_FORMAT f, DXGI_FORMAT of) { Safe([&] { AttachDevice(d); AttachQueue(q); }); auto r = o(s, sr, d, a, q, i, out, hwnd, f, of); Safe([&] { RegisterLegacy12(s, d, q, i, hwnd, f); }); return r; }
};
template<class Tag> void ExportHook(HMODULE m, const std::string& name) {
    if (auto p = GetProcAddress(m, name.c_str())) { HMODULE pin{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(p), &pin); if (!Hook<Tag>(reinterpret_cast<void*>(p))) Status("cannot attach export " + name); }
}
void InstallLegacy(HMODULE m) {
    ExportHook<Legacy11Input>(m, "?setInputFrameBuffer@DX11WeaverBase@SR@@QEAAXPEAUID3D11ShaderResourceView@@@Z");
    ExportHook<Legacy11Get>(m, "?getFrameBuffer@DX11WeaverBase@SR@@QEAAPEAUID3D11RenderTargetView@@XZ");
    ExportHook<Legacy11Context>(m, "?setContext@DX11WeaverBase@SR@@QEAAXPEAUID3D11DeviceContext@@@Z");
    ExportHook<LegacyWindow<false>>(m, "?setWindowHandle@DX11WeaverBase@SR@@QEAAXPEAUHWND__@@@Z");
    ExportHook<LegacyWeave<false, false>>(m, "?weave@DX11WeaverBase@SR@@QEAAXII@Z");
    ExportHook<LegacyWeave<false, true>>(m, "?weave@DX11WeaverBase@SR@@QEAAXIIII@Z");
    ExportHook<Legacy12Input>(m, "?setInputFrameBuffer@DX12WeaverBase@SR@@QEAAXPEAUID3D12Resource@@@Z");
    ExportHook<Legacy12InputFormat>(m, "?setInputFrameBuffer@DX12WeaverBase@SR@@QEAAXPEAUID3D12Resource@@W4DXGI_FORMAT@@@Z");
    ExportHook<Legacy12Get>(m, "?getFrameBuffer@DX12WeaverBase@SR@@QEAAPEAUID3D12Resource@@XZ");
    ExportHook<Legacy12List>(m, "?setCommandList@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@@Z");
    ExportHook<LegacyWindow<true>>(m, "?setWindowHandle@DX12WeaverBase@SR@@QEAAXPEAUHWND__@@@Z");
    ExportHook<LegacyWeave<true, false>>(m, "?weave@DX12WeaverBase@SR@@QEAAXII@Z");
    ExportHook<LegacyWeave<true, true>>(m, "?weave@DX12WeaverBase@SR@@QEAAXIIII@Z");
    ExportHook<LegacyWeaveList<false>>(m, "?weave@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@II@Z");
    ExportHook<LegacyWeaveList<true>>(m, "?weave@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@IIII@Z");
    for (const auto name : {"DX11Weaver", "PredictingDX11Weaver"}) {
        const auto stem = std::string("??0") + name + "@SR@@QEAA@AEAVSRContext@1@PEAUID3D11Device@@PEAUID3D11DeviceContext@@II";
        ExportHook<Ctor11<false, false>>(m, stem + "@Z"); ExportHook<Ctor11<false, true>>(m, stem + "PEAUHWND__@@@Z");
        ExportHook<Ctor11<true, false>>(m, stem + "W4DXGI_FORMAT@@@Z"); ExportHook<Ctor11<true, true>>(m, stem + "W4DXGI_FORMAT@@PEAUHWND__@@@Z");
        ExportHook<LegacyDestroy>(m, std::string("??1") + name + "@SR@@QEAA@XZ");
    }
    ExportHook<LegacyDestroy>(m, "??1DX11WeaverBase@SR@@QEAA@XZ");
    for (const auto name : {"DX12Weaver", "PredictingDX12Weaver"}) {
        const auto stem = std::string("??0") + name + "@SR@@QEAA@AEAVSRContext@1@PEAUID3D12Device@@PEAUID3D12CommandAllocator@@PEAUID3D12CommandQueue@@PEAUID3D12Resource@@4";
        ExportHook<Ctor12<0>>(m, stem + "@Z"); ExportHook<Ctor12<1>>(m, stem + "PEAUHWND__@@@Z");
        ExportHook<Ctor12<2>>(m, stem + "PEAUHWND__@@W4DXGI_FORMAT@@@Z"); ExportHook<Ctor12<3>>(m, stem + "PEAUHWND__@@W4DXGI_FORMAT@@5@Z");
        ExportHook<LegacyDestroy>(m, std::string("??1") + name + "@SR@@QEAA@XZ");
    }
    ExportHook<LegacyDestroy>(m, "??1DX12WeaverBase@SR@@QEAA@XZ");
}

#include "ModernRecovery.inl"

bool CopyNativeGpuHooksPayload(const void* value, NativeGpuHooks* copy) noexcept {
    __try {
        if (static_cast<const NativeGpuHooks*>(value)->size != sizeof(NativeGpuHooks)) return false;
        std::memcpy(copy, value, sizeof(NativeGpuHooks)); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ValidNativeGpuHooks(const NativeGpuHooks& hooks) {
    if (hooks.size != sizeof(NativeGpuHooks) || hooks.version != 1 || !hooks.moduleCount || hooks.moduleCount > hooks.modules.size() ||
        !hooks.siteCount || hooks.siteCount > hooks.sites.size()) return false;
    for (uint32_t i = 0; i < hooks.moduleCount; ++i) {
        const auto& module = hooks.modules[i];
        if (!module.path[0] || !std::wmemchr(module.path, 0, std::size(module.path)) || !module.imageSize) return false;
    }
    bool present{}, execute{}, release{};
    for (uint32_t i = 0; i < hooks.siteCount; ++i) {
        const auto& site = hooks.sites[i];
        if (site.module >= hooks.moduleCount || site.reserved || !site.rva || site.rva >= hooks.modules[site.module].imageSize ||
            static_cast<uint32_t>(site.kind) > static_cast<uint32_t>(NativeGpuHook::QueueRelease)) return false;
        present |= site.kind == NativeGpuHook::Present || site.kind == NativeGpuHook::Present1;
        execute |= site.kind == NativeGpuHook::QueueExecute;
        release |= site.kind == NativeGpuHook::QueueRelease;
        for (uint32_t previous = 0; previous < i; ++previous) {
            const auto& other = hooks.sites[previous];
            if (other.module == site.module && other.rva == site.rva && other.kind != site.kind) return false;
        }
    }
    return present && execute == release;
}

DWORD WINAPI Worker(void* ready) {
    SetEvent(static_cast<HANDLE>(ready));
    CloseHandle(static_cast<HANDLE>(ready));
    for (;;) {
        if (active) { ScanModules(); Safe([] { core->Poll12(); }); }
        WaitForSingleObject(workerWake, 25);
    }
}
}
}

extern "C" __declspec(dllexport) DWORD WINAPI LAN3DConfigure(void* value) {
    using namespace lan3d; using namespace lan3d::capture;
    if (!value) return ERROR_INVALID_PARAMETER;
    const auto c = *static_cast<const CaptureConfig*>(value);
    if (c.size != sizeof(CaptureConfig) || c.version != 1 || (c.reserved & ~CaptureGpuOutput) || c.maxFps > 240) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(controlMutex); config = c; if (core) core->Configure(c); return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI LAN3DSetGpuHooks(void* value) {
    using namespace lan3d; using namespace lan3d::capture;
    if (!value) return ERROR_INVALID_PARAMETER;
    NativeGpuHooks candidate;
    if (!CopyNativeGpuHooksPayload(value, &candidate) || !ValidNativeGpuHooks(candidate)) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(controlMutex);
    if (active) return ERROR_BUSY;
    nativeHooks = candidate; nativeHooksConfigured = true;
    nativeSitesInstalled.fill(false); nativeModules.fill(nullptr);
    nativePresentReady = nativeQueueReady = false; nativeStatus.clear(); blockedCaptureStatus.clear();
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI LAN3DStart(void*) {
    using namespace lan3d; using namespace lan3d::capture;
    std::lock_guard lock(controlMutex);
    if (!nativeHooksConfigured) { Status("capture requires a validated external GPU hook profile before Start"); return ERROR_NOT_READY; }
    // A partial installation is never reported as a successful restart. Hooks
    // cannot safely be rolled back while another thread may be executing them.
    if (core && !initialized) return initializationError;
    if (!core) {
        bus = new FrameBus; std::string error; if (!bus->Open(GetCurrentProcessId(), true, error)) { delete bus; bus = nullptr; return ERROR_OPEN_FAILED; }
        gpuBus = new GpuFrameBus;
        if (!gpuBus->Open(GetCurrentProcessId(), true, error)) { delete gpuBus; gpuBus = nullptr; return ERROR_OPEN_FAILED; }
        core = new Core([](const Frame& frame) { bus->Publish(frame); }, Status);
        core->SetGpuOutput([](const GpuFrame& frame) { return gpuBus->Publish(frame); },
            [](uint64_t sequence) { return gpuBus->IsReleased(sequence); });
        core->Configure(config);
        const auto mh = MH_Initialize(); if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return initializationError;
        InstallNativeGpuHooks();
        leiaSlots = new LeiaSlots;
        HMODULE self{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&LAN3DStart), &self);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!ready) return initializationError = GetLastError();
        workerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!workerWake) { initializationError = GetLastError(); CloseHandle(ready); return initializationError; }
        HANDLE workerReady{};
        if (!DuplicateHandle(GetCurrentProcess(), ready, GetCurrentProcess(), &workerReady, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            initializationError = GetLastError(); CloseHandle(workerWake); workerWake = nullptr; CloseHandle(ready); return initializationError;
        }
        worker = CreateThread(nullptr, 0, Worker, workerReady, 0, nullptr);
        if (!worker) { initializationError = GetLastError(); CloseHandle(workerWake); workerWake = nullptr; CloseHandle(workerReady); CloseHandle(ready); return initializationError; }
        const auto waited = WaitForSingleObject(ready, 5000); CloseHandle(ready);
        if (waited != WAIT_OBJECT_0) return initializationError = ERROR_TIMEOUT;
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        const bool loadW = Hook<LoadLibraryHook<true, false>>(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryW")));
        const bool loadA = Hook<LoadLibraryHook<false, false>>(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryA")));
        const bool loadExW = Hook<LoadLibraryHook<true, true>>(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryExW")));
        const bool loadExA = Hook<LoadLibraryHook<false, true>>(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryExA")));
        if (!loadW || !loadA || !loadExW || !loadExA) return initializationError = ERROR_NOT_SUPPORTED;
        initialized = true; initializationError = ERROR_SUCCESS;
    }
    active = true; core->SetEnabled(true); ScanModules(); return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI LAN3DStop(void*) {
    using namespace lan3d::capture;
    active = false; if (core) core->SetEnabled(false); Status("capture stopped; hooks remain resident until process exit"); return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

