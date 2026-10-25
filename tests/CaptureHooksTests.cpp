#include "capture/CaptureApi.h"
#include "capture/NativeGpuHooks.h"
#include "common/FrameBus.h"
#include <sr/weaver/dx11weaver.h>
#include <sr/weaver/dx12weaver.h>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <array>
#include <algorithm>
#include <atomic>
#include <MinHook.h>
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool x, const char* message) { if (!x) { std::cerr << "FAIL: " << message << std::endl; throw std::runtime_error(message); } }
void Hr(HRESULT h, const char* message) { if (FAILED(h)) { std::cerr << "FAIL: " << message << " HRESULT=" << std::hex << h << std::dec << std::endl; throw std::runtime_error(message); } }
template<class F> F Resolve(HMODULE m, const char* n) {
    auto p = GetProcAddress(m, n);
    // Instrumentation belongs to the fake implementation, not the public
    // factory/legacy exports exposed by the production bridge boundary.
    if (!p && std::strncmp(n, "FakeLeia", 8) == 0) if (auto implementation = GetModuleHandleW(L"FakeLeiaRuntime.dll")) p = GetProcAddress(implementation, n);
    Check(p != nullptr, n); return reinterpret_cast<F>(p);
}
using Call = DWORD(WINAPI*)(void*);
lan3d::NativeGpuHooks nativeGpuHooks;
constexpr const char* Create11 = "?CreateDX11Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D11DeviceContext@@PEAUHWND__@@PEAPEAVIDX11Weaver1@1@@Z";
constexpr const char* Create12 = "?CreateDX12Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D12Device@@PEAUHWND__@@PEAPEAVIDX12Weaver1@1@@Z";
void StartCapture(const wchar_t* path, lan3d::FrameBus& bus, Call& stop, Call& start) {
    auto capture = LoadLibraryW(path); Check(capture != nullptr, "capture LoadLibrary");
    auto configure = Resolve<Call>(capture, "LAN3DConfigure");
    auto setGpuHooks = Resolve<Call>(capture, "LAN3DSetGpuHooks");
    Check(setGpuHooks(&nativeGpuHooks) == ERROR_SUCCESS, "capture native GPU hook profile");
    start = Resolve<Call>(capture, "LAN3DStart"); stop = Resolve<Call>(capture, "LAN3DStop");
    lan3d::CaptureConfig config; config.maxFps = 0; Check(configure(&config) == ERROR_SUCCESS && start(nullptr) == ERROR_SUCCESS, "capture start");
    std::string error; Check(bus.Open(GetCurrentProcessId(), false, error), "bus open");
}
void WaitForRuntimeHooks(lan3d::FrameBus& bus) {
    // These modes test factories called after installation. Runtime loads now
    // notify the worker asynchronously; wait for its actual installation result.
    // The late-attach modes intentionally do not use this readiness barrier.
    const auto deadline = GetTickCount64() + 5000;
    std::string status;
    do {
        status = bus.Status();
        if (status == "SimulatedRealityDirectX hooks installed; observing existing and new weavers") return;
        Sleep(2);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("asynchronous runtime hook installation timed out: " + status);
}
// An unrelated graphics consumer observes creation events in the host. A
// capture probe that creates even a hidden chain can replace that consumer's
// active queue or re-enter its presentation lock. Only readbacks/fences may be
// allocated during these already-created-weaver scenarios.
enum class HostCreation : size_t { Device, Factory, Queue, Allocator, List, Swap, Count };
std::array<std::atomic<unsigned>, size_t(HostCreation::Count)> hostCreations{};
std::atomic<bool> observeHostCreations{};
std::atomic<bool> traceHostCreations{};
void Observe(HostCreation kind) {
    if (!observeHostCreations || ++hostCreations[size_t(kind)] != 1 || !traceHostCreations) return;
    // Keep an unexpected creation attributable: DXGI may allocate work lazily
    // during the fixture's Present, whereas capture must create none itself.
    void* stack[16]{};
    const auto count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(stack)), stack, nullptr);
    std::cerr << "Unexpected host creation kind=" << size_t(kind) << " stack:" << std::endl;
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{}; char path[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(stack[i]), &module)) GetModuleFileNameA(module, path, MAX_PATH);
        const auto* name = std::strrchr(path, '\\'); name = name ? name + 1 : path;
        std::cerr << "  " << name << "+0x" << std::hex <<
            (reinterpret_cast<uintptr_t>(stack[i]) - reinterpret_cast<uintptr_t>(module)) << std::dec << std::endl;
    }
}
template<class Signature, HostCreation Kind, unsigned Slot = 0> struct CreationHook;
template<HostCreation Kind, unsigned Slot, class R, class... A>
struct CreationHook<R(WINAPI*)(A...), Kind, Slot> {
    using Fn = R(WINAPI*)(A...);
    static inline Fn original{};
    static R WINAPI Call(A... args) { Observe(Kind); return original(args...); }
};
std::vector<void*> creationSites;
template<class Hook> void ObserveCreation(void* site) {
    Check(site != nullptr, "creation observer target");
    if (std::find(creationSites.begin(), creationSites.end(), site) != creationSites.end()) return;
    Check(MH_CreateHook(site, reinterpret_cast<void*>(Hook::Call), reinterpret_cast<void**>(&Hook::original)) == MH_OK, "creation observer install");
    Check(MH_EnableHook(site) == MH_OK, "creation observer enable");
    creationSites.push_back(site);
}
void ObserveHostGraphics(IDXGIFactory2* factory, ID3D12Device* device = nullptr) {
    const auto init = MH_Initialize(); Check(init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED, "observer MinHook init");
    const auto dxgi = GetModuleHandleW(L"dxgi.dll");
    using Factory = HRESULT(WINAPI*)(REFIID, void**);
    using Factory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    ObserveCreation<CreationHook<Factory, HostCreation::Factory, 0>>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")));
    ObserveCreation<CreationHook<Factory, HostCreation::Factory, 1>>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    ObserveCreation<CreationHook<Factory2, HostCreation::Factory>>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
    if (const auto d11 = GetModuleHandleW(L"d3d11.dll"))
        ObserveCreation<CreationHook<decltype(&D3D11CreateDevice), HostCreation::Device>>(reinterpret_cast<void*>(GetProcAddress(d11, "D3D11CreateDevice")));
    if (const auto d12 = GetModuleHandleW(L"d3d12.dll"))
        ObserveCreation<CreationHook<decltype(&D3D12CreateDevice), HostCreation::Device>>(reinterpret_cast<void*>(GetProcAddress(d12, "D3D12CreateDevice")));
    auto** table = *reinterpret_cast<void***>(factory);
    using Swap = HRESULT(WINAPI*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using SwapHwnd = HRESULT(WINAPI*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using SwapCore = HRESULT(WINAPI*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    using SwapComposition = HRESULT(WINAPI*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    ObserveCreation<CreationHook<Swap, HostCreation::Swap>>(table[10]);
    ObserveCreation<CreationHook<SwapHwnd, HostCreation::Swap>>(table[15]);
    ObserveCreation<CreationHook<SwapCore, HostCreation::Swap>>(table[16]);
    ObserveCreation<CreationHook<SwapComposition, HostCreation::Swap>>(table[24]);
    if (device) {
        table = *reinterpret_cast<void***>(device);
        using Queue = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
        using Allocator = HRESULT(WINAPI*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
        using List = HRESULT(WINAPI*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
        using List1 = HRESULT(WINAPI*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
        ObserveCreation<CreationHook<Queue, HostCreation::Queue>>(table[8]);
        ObserveCreation<CreationHook<Allocator, HostCreation::Allocator>>(table[9]);
        ObserveCreation<CreationHook<List, HostCreation::List>>(table[12]);
        ComPtr<ID3D12Device4> device4;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))))
            ObserveCreation<CreationHook<List1, HostCreation::List>>((*reinterpret_cast<void***>(device4.Get()))[51]);
    }
    observeHostCreations = true;
    // Verify this independent observer sees genuine creation calls before the
    // measured attachment interval, then establish its zero baseline.
    {
        ComPtr<IDXGIFactory2> calibrationFactory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&calibrationFactory)), "creation observer factory calibration");
        Check(hostCreations[size_t(HostCreation::Factory)] > 0, "factory creation observer is inactive");
        if (device) {
            D3D12_COMMAND_QUEUE_DESC desc{}; ComPtr<ID3D12CommandQueue> calibrationQueue;
            Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&calibrationQueue)), "creation observer queue calibration");
            Check(hostCreations[size_t(HostCreation::Queue)] > 0, "queue creation observer is inactive");
        }
    }
    for (auto& count : hostCreations) count = 0;
    traceHostCreations = true;
}
void CheckHostGraphicsUnchanged() {
    constexpr const char* names[]{"device", "factory", "queue", "allocator", "command list", "swapchain"};
    for (size_t i = 0; i < hostCreations.size(); ++i) if (const auto count = hostCreations[i].load()) {
        const auto error = "capture created " + std::to_string(count) + " host " + names[i] + " object(s)";
        std::cerr << error << std::endl; throw std::runtime_error(error);
    }
}
void FrameCheck(lan3d::FrameBus& bus, IDXGISwapChain1* swap, uint64_t after, uint8_t r, uint8_t b) {
    CheckHostGraphicsUnchanged();
    lan3d::Frame frame; bool found{};
    for (unsigned i = 0; i < 400; ++i) { DXGI_PRESENT_PARAMETERS pp{}; swap->Present1(0, 0, &pp); if (bus.ReadLatest(frame, after)) { found = true; break; } Sleep(2); }
    if (!found) { const auto message = "capture timeout: " + bus.Status(); std::cerr << message << std::endl; throw std::runtime_error(message); }
    Check(frame.width == 8 && frame.height == 4, "wrong captured dimensions");
    Check(frame.kind == lan3d::ContentKind::LeiaSbs, "captured SBS lost LeiaSR metadata");
    for (size_t i = 0; i < frame.bgra.size(); i += 4) Check(frame.bgra[i] == b && frame.bgra[i + 1] == 71 && frame.bgra[i + 2] == r && frame.bgra[i + 3] == 255, "captured wrong SBS pixels");
}
void NoFrameCheck(lan3d::FrameBus& bus, IDXGISwapChain1* swap, uint64_t after = 0) {
    CheckHostGraphicsUnchanged();
    for (unsigned i = 0; i < 10; ++i) {
        DXGI_PRESENT_PARAMETERS pp{}; swap->Present1(0, 0, &pp);
        lan3d::Frame frame; Check(!bus.ReadLatest(frame, after), "unsupported/unobserved capture produced a frame"); Sleep(2);
    }
}
std::vector<uint8_t> Pixels(uint8_t r, uint8_t b) { std::vector<uint8_t> p(8 * 4 * 4); for (size_t i = 0; i < p.size(); i += 4) { p[i] = r; p[i + 1] = 71; p[i + 2] = b; p[i + 3] = 255; } return p; }
ComPtr<IDXGISwapChain1> Swap(IDXGIFactory2* factory, IUnknown* device, HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = desc.Height = 16; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferCount = 2; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; Hr(factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &swap), "test swap chain"); return swap;
}
void Test11(HMODULE runtime, lan3d::FrameBus& bus, HWND hwnd, bool legacy, Call& stop, Call& start, const wchar_t* lateCapture, bool inputMode) {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "DX11 WARP debug");
    ComPtr<IDXGIFactory2> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); auto swap = Swap(factory.Get(), device.Get(), hwnd);
    auto mask = Pixels(11, 13), final = Pixels(202, 32);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 8; desc.Height = 4; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; Hr(device->CreateTexture2D(&desc, nullptr, &texture), "source");
    ComPtr<ID3D11ShaderResourceView> srv; Hr(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "SRV");
    if (!legacy) {
        auto factoryFn = Resolve<WeaverErrorCode(*)(SR::SRContext*, ID3D11DeviceContext*, HWND, SR::IDX11Weaver1**)>(runtime, Create11);
        auto invalidOutput = reinterpret_cast<SR::IDX11Weaver1*>(1);
        Check(factoryFn(reinterpret_cast<SR::SRContext*>(1), context.Get(), hwnd, &invalidOutput) != WeaverSuccess, "factory11 failure forwarding");
        SR::IDX11Weaver1* weaver{}; Check(factoryFn(nullptr, context.Get(), hwnd, &weaver) == WeaverSuccess, "factory11");
        Check(weaver->query<SR::IDX11Weaver1>() == weaver, "hook changed object identity/query");
        Check(weaver->query<SR::IWeaverBase1>() == static_cast<SR::IWeaverBase1*>(weaver), "virtual-base query adjustment broken");
        weaver->setInputViewTexture(srv.Get(), 4, 4, desc.Format); weaver->setLatency(123456); Check(weaver->getLatency() == 123456, "unhooked API semantics changed");
        context->UpdateSubresource(texture.Get(), 0, nullptr, mask.data(), 32, 0); weaver->weave();
        context->UpdateSubresource(texture.Get(), 0, nullptr, final.data(), 32, 0); weaver->weave(); context->Flush(); FrameCheck(bus, swap.Get(), 0, 202, 32);
        lan3d::Frame first; Check(bus.ReadLatest(first, 0), "first frame");
        Check(stop(nullptr) == ERROR_SUCCESS, "stop"); weaver->weave(); context->Flush(); swap->Present(0, 0); Sleep(10);
        lan3d::Frame unexpected; Check(!bus.ReadLatest(unexpected, first.sequence), "Stop published a frame"); Check(start(nullptr) == ERROR_SUCCESS, "restart");
        weaver->weave(); context->Flush(); FrameCheck(bus, swap.Get(), first.sequence, 202, 32); weaver->destroy();
        Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == 4, "original weave forwarding count incorrect");
    } else {
        // Resolve the documented legacy exports without linking the actual SR
        // runtime. The fake constructor does not inspect the SRContext reference.
        using Ctor = void*(*)(void*, SR::SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned, DXGI_FORMAT, HWND);
        auto ctor = Resolve<Ctor>(runtime, "??0PredictingDX11Weaver@SR@@QEAA@AEAVSRContext@1@PEAUID3D11Device@@PEAUID3D11DeviceContext@@IIW4DXGI_FORMAT@@PEAUHWND__@@@Z");
        alignas(SR::PredictingDX11Weaver) std::byte object[sizeof(SR::PredictingDX11Weaver)]{};
        alignas(SR::SRContext) std::byte fakeContext[sizeof(SR::SRContext)]{};
        ctor(object, *reinterpret_cast<SR::SRContext*>(fakeContext), device.Get(), context.Get(), 8, 4, desc.Format, hwnd);
        auto getter = Resolve<ID3D11RenderTargetView*(*)(void*)>(runtime, "?getFrameBuffer@DX11WeaverBase@SR@@QEAAPEAUID3D11RenderTargetView@@XZ");
        auto weave = Resolve<void(*)(void*, unsigned, unsigned)>(runtime, "?weave@DX11WeaverBase@SR@@QEAAXII@Z");
        ComPtr<ID3D11Resource> target; getter(object)->GetResource(&target);
        if (lateCapture) {
            context->UpdateSubresource(target.Get(), 0, nullptr, mask.data(), 32, 0);
            if (inputMode) {
                Resolve<void(*)(void*, ID3D11ShaderResourceView*)>(runtime, "?setInputFrameBuffer@DX11WeaverBase@SR@@QEAAXPEAUID3D11ShaderResourceView@@@Z")(object, srv.Get());
                Check(getter(object) == nullptr, "external input must release internal framebuffer");
                context->UpdateSubresource(texture.Get(), 0, nullptr, mask.data(), 32, 0);
            }
            weave(object, 8, 4); context->Flush(); swap->Present(0, 0);
            Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == 1, "pre-attach legacy11 weave");
            StartCapture(lateCapture, bus, stop, start);
            lan3d::Frame unexpected; Check(!bus.ReadLatest(unexpected, 0), "hot attach published a stale frame");
            if (inputMode) { weave(object, 8, 4); context->Flush(); NoFrameCheck(bus, swap.Get()); }
        }
        if (inputMode) {
            // An external input configured before attachment is inaccessible
            // through the getter. A later public setter must recover capture.
            Resolve<void(*)(void*, ID3D11ShaderResourceView*)>(runtime, "?setInputFrameBuffer@DX11WeaverBase@SR@@QEAAXPEAUID3D11ShaderResourceView@@@Z")(object, srv.Get());
            context->UpdateSubresource(texture.Get(), 0, nullptr, final.data(), 32, 0);
        } else context->UpdateSubresource(target.Get(), 0, nullptr, final.data(), 32, 0);
        // No post-attach getter or constructor: weave must discover the
        // pre-existing object through its public framebuffer getter.
        weave(object, 8, 4); context->Flush(); FrameCheck(bus, swap.Get(), 0, 202, 32);
        if (lateCapture && !inputMode) {
            lan3d::Frame first; Check(bus.ReadLatest(first, 0), "late11 first frame");
            auto setContext = Resolve<void(*)(void*, ID3D11DeviceContext*)>(runtime, "?setContext@DX11WeaverBase@SR@@QEAAXPEAUID3D11DeviceContext@@@Z");
            ComPtr<ID3D11DeviceContext> deferred; Hr(device->CreateDeferredContext(0, &deferred), "test deferred context");
            setContext(object, deferred.Get()); getter(object); weave(object, 8, 4); NoFrameCheck(bus, swap.Get(), first.sequence);
            setContext(object, nullptr); getter(object); weave(object, 8, 4); NoFrameCheck(bus, swap.Get(), first.sequence);
            // Re-observing a framebuffer must not replace an explicit deferred
            // or null context with a guessed immediate context.
            setContext(object, context.Get()); weave(object, 8, 4); context->Flush(); FrameCheck(bus, swap.Get(), first.sequence, 202, 32);
        }
        Resolve<void(*)(void*)>(runtime, "??1PredictingDX11Weaver@SR@@QEAA@XZ")(object);
        Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == (lateCapture ? (inputMode ? 3u : 5u) : 1u), "nested legacy overload hooked twice");
    }
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaDestroyed")() == 1, "destroy forwarding broken");
    ComPtr<ID3D11InfoQueue> debug; Hr(device.As(&debug), "debug11");
    for (uint64_t i = 0; i < debug->GetNumStoredMessages(); ++i) { SIZE_T size{}; debug->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size); auto m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); debug->GetMessage(i, m, &size); if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription); }
}
void Test12(HMODULE runtime, lan3d::FrameBus& bus, HWND hwnd, bool legacy, Call& stop, Call& start, const wchar_t* lateCapture, bool inputMode) {
    ComPtr<ID3D12Debug> debug; Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug12"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory12"); ComPtr<IDXGIAdapter> adapter; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "warp12");
    ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device12");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue12"); auto swap = Swap(factory.Get(), queue.Get(), hwnd);
    SR::IDX12Weaver1* weaver{};
    if (!legacy) {
        auto factoryFn = Resolve<WeaverErrorCode(*)(SR::SRContext*, ID3D12Device*, HWND, SR::IDX12Weaver1**)>(runtime, Create12);
        auto invalidOutput = reinterpret_cast<SR::IDX12Weaver1*>(1);
        Check(factoryFn(reinterpret_cast<SR::SRContext*>(1), device.Get(), hwnd, &invalidOutput) != WeaverSuccess, "factory12 failure forwarding");
        Check(factoryFn(nullptr, device.Get(), hwnd, &weaver) == WeaverSuccess, "factory12");
        Check(weaver->query<SR::IDX12Weaver1>() == weaver, "DX12 query identity");
    }
    ComPtr<ID3D12CommandAllocator> allocator; Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator12");
    ComPtr<ID3D12GraphicsCommandList> list; Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list12");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 8; desc.Height = 4; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> input; Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&input)), "input12");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t bytes{}; device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload; Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload12");
    void* data{}; Hr(upload->Map(0, nullptr, &data), "map12"); auto pixels = Pixels(201, 31); for (unsigned y = 0; y < 4; ++y) memcpy(static_cast<uint8_t*>(data) + size_t(y) * footprint.Footprint.RowPitch, pixels.data() + y * 32, 32); upload->Unmap(0, nullptr);
    alignas(SR::PredictingDX12Weaver) std::byte object[sizeof(SR::PredictingDX12Weaver)]{};
    if (legacy) {
        using Ctor = void*(*)(void*, SR::SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource*, ID3D12Resource*, HWND);
        auto ctor = Resolve<Ctor>(runtime, "??0PredictingDX12Weaver@SR@@QEAA@AEAVSRContext@1@PEAUID3D12Device@@PEAUID3D12CommandAllocator@@PEAUID3D12CommandQueue@@PEAUID3D12Resource@@4PEAUHWND__@@@Z");
        alignas(SR::SRContext) std::byte fakeContext[sizeof(SR::SRContext)]{};
        ctor(object, *reinterpret_cast<SR::SRContext*>(fakeContext), device.Get(), allocator.Get(), queue.Get(), input.Get(), nullptr, hwnd);
        auto getter = Resolve<ID3D12Resource*(*)(void*)>(runtime, "?getFrameBuffer@DX12WeaverBase@SR@@QEAAPEAUID3D12Resource@@XZ"); Check(getter(object) == input.Get(), "legacy12 getter");
        Resolve<void(*)(void*, ID3D12Resource*, DXGI_FORMAT)>(runtime, "?setInputFrameBuffer@DX12WeaverBase@SR@@QEAAXPEAUID3D12Resource@@W4DXGI_FORMAT@@@Z")(object, input.Get(), desc.Format);
        Resolve<void(*)(void*, ID3D12GraphicsCommandList*)>(runtime, "?setCommandList@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@@Z")(object, list.Get());
    } else { weaver->setInputViewTexture(input.Get(), 4, 4, desc.Format); weaver->setCommandList(list.Get()); }
    if (lateCapture) {
        Resolve<void(*)(void*, unsigned, unsigned)>(runtime, "?weave@DX12WeaverBase@SR@@QEAAXII@Z")(object, 8, 4);
        Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == 1, "pre-attach legacy12 weave");
        StartCapture(lateCapture, bus, stop, start);
        lan3d::Frame unexpected; Check(!bus.ReadLatest(unexpected, 0), "hot attach published a stale frame");
        if (inputMode) Resolve<void(*)(void*, ID3D12Resource*, DXGI_FORMAT)>(runtime, "?setInputFrameBuffer@DX12WeaverBase@SR@@QEAAXPEAUID3D12Resource@@W4DXGI_FORMAT@@@Z")(object, input.Get(), desc.Format);
        // Only command lists observed after attachment can safely track resource
        // barriers. Reuse the existing list, then record the transition below.
        Resolve<void(*)(void*, ID3D12GraphicsCommandList*)>(runtime, "?setCommandList@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@@Z")(object, list.Get());
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint; dst.pResource = input.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence12"); uint64_t fenceValue{};
    auto waitGpu = [&] {
        Hr(queue->Signal(fence.Get(), ++fenceValue), "signal12");
        for (unsigned i = 0; i < 1000 && fence->GetCompletedValue() < fenceValue; ++i) Sleep(1);
        Check(fence->GetCompletedValue() == fenceValue, "GPU12 timeout");
    };
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    for (unsigned frame = 0; frame < 2; ++frame) {
        if (frame) {
            Hr(allocator->Reset(), "allocator12 reset"); Hr(list->Reset(allocator.Get(), nullptr), "list12 reset");
            barrier.Transition = {input.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
            list->ResourceBarrier(1, &barrier);
        }
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition = {input.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}; list->ResourceBarrier(1, &barrier);
        if (legacy) {
            Resolve<void(*)(void*, unsigned, unsigned)>(runtime, "?weave@DX12WeaverBase@SR@@QEAAXII@Z")(object, 8, 4);
            Resolve<void(*)(void*, ID3D12GraphicsCommandList*, unsigned, unsigned, unsigned, unsigned)>(runtime, "?weave@DX12WeaverBase@SR@@QEAAXPEAUID3D12GraphicsCommandList@@IIII@Z")(object, list.Get(), 8, 4, 0, 0);
        } else weaver->weave();
        Hr(list->Close(), "close12"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); waitGpu();
        // The first actual submit establishes the recording object's identity
        // at the queue boundary. Copying before that proof must be skipped.
        if (!frame) NoFrameCheck(bus, swap.Get());
        else FrameCheck(bus, swap.Get(), 0, 201, 31);
    }
    waitGpu(); // FrameCheck also submits DXGI presentation work.
    if (legacy) Resolve<void(*)(void*)>(runtime, "??1PredictingDX12Weaver@SR@@QEAA@XZ")(object); else weaver->destroy();
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaDestroyed")() == 1, "destroy12");
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == (legacy ? (lateCapture ? 5u : 4u) : 2u), "DX12 original weave forwarding count");
    ComPtr<ID3D12InfoQueue> info; Hr(device.As(&info), "info12"); for (uint64_t i = 0; i < info->GetNumStoredMessages(); ++i) { SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<uint8_t> text(size); auto m = reinterpret_cast<D3D12_MESSAGE*>(text.data()); info->GetMessage(i, m, &size); if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << "D3D12 debug error: " << m->pDescription << std::endl; throw std::runtime_error(m->pDescription); } }
}
using SetterCounts = std::array<unsigned, 4>;
SetterCounts Counts(HMODULE runtime) {
    return {Resolve<unsigned(*)()>(runtime, "FakeLeiaFactories")(), Resolve<unsigned(*)()>(runtime, "FakeLeiaInputs")(),
        Resolve<unsigned(*)()>(runtime, "FakeLeiaContexts")(), Resolve<unsigned(*)()>(runtime, "FakeLeiaLists")()};
}
uint64_t Latest(lan3d::FrameBus& bus) { lan3d::Frame f; Check(bus.ReadLatest(f, 0), "expected captured frame"); return f.sequence; }
template<class T> void CheckIdentity(T* weaver, bool pimpl) {
    Check(weaver->template query<T>() == weaver, "modern identity changed");
    Check(weaver->template query<SR::IWeaverBase1>() == static_cast<SR::IWeaverBase1*>(weaver), "modern virtual base adjustment changed");
    Check(weaver->template query<SR::IDestroyable>() == static_cast<SR::IDestroyable*>(weaver), "modern destroy adjustment changed");
    if (pimpl) Check(dynamic_cast<void*>(weaver) != static_cast<void*>(weaver), "pimpl fixture must have a nonzero typed-interface offset");
    weaver->setLatency(123456); Check(weaver->getLatency() == 123456, "modern unrelated API semantics changed");
}
void ModernLate11(HMODULE runtime, lan3d::FrameBus& bus, HWND hwnd, Call& stop, Call& start, const wchar_t* capture, bool pimpl) {
    // Resolve a public forwarder before looking up test instrumentation: Windows
    // need not load its implementation DLL until GetProcAddress follows it.
    const auto create = Resolve<WeaverErrorCode(*)(SR::SRContext*, ID3D11DeviceContext*, HWND, SR::IDX11Weaver1**)>(runtime, Create11);
    Resolve<void(*)(unsigned)>(runtime, "FakeLeiaLayout")(pimpl ? 1 : 0);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context, deferred;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "modern11 device");
    Hr(device->CreateDeferredContext(0, &deferred), "modern11 deferred");
    ComPtr<IDXGIFactory2> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "modern11 factory"); auto swap = Swap(factory.Get(), device.Get(), hwnd);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 8; desc.Height = 4; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; Hr(device->CreateTexture2D(&desc, nullptr, &texture), "modern11 input");
    ComPtr<ID3D11ShaderResourceView> srv; Hr(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "modern11 SRV");
    std::array<SR::IDX11Weaver1*, 4> weavers{};
    const std::array<ID3D11DeviceContext*, 4> initialContexts{context.Get(), context.Get(), nullptr, deferred.Get()};
    unsigned expectedCalls{};
    auto weave = [&](SR::IDX11Weaver1* w) { w->weave(); ++expectedCalls; };
    auto update = [&](uint8_t r, uint8_t b) { const auto pixels = Pixels(r, b); context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), 32, 0); };
    update(11, 13);
    for (size_t i = 0; i < weavers.size(); ++i) {
        Check(create(nullptr, initialContexts[i], hwnd, &weavers[i]) == WeaverSuccess, "modern11 pre-attach factory");
        CheckIdentity(weavers[i], pimpl);
        weavers[i]->setInputViewTexture(i == 1 ? nullptr : srv.Get(), 4, 4, desc.Format);
        weave(weavers[i]);
    }
    context->Flush(); swap->Present(0, 0);
    const auto initialCounts = Counts(runtime);
    ObserveHostGraphics(factory.Get());
    StartCapture(capture, bus, stop, start);
    NoFrameCheck(bus, swap.Get());
    // No factory/input/context calls from here through restart. The texture,
    // immediate context and every weaver already existed before DLL loading.
    update(202, 32); weave(weavers[0]); context->Flush(); FrameCheck(bus, swap.Get(), 0, 202, 32);
    auto sequence = Latest(bus);
    update(203, 33); weave(weavers[0]); context->Flush(); FrameCheck(bus, swap.Get(), sequence, 203, 33); sequence = Latest(bus);
    // A readback recorded before Stop but not decided by Present must never be
    // replayed after Start on the same surviving object.
    update(44, 45); weave(weavers[0]); context->Flush();
    Check(stop(nullptr) == ERROR_SUCCESS, "modern11 stop");
    update(55, 56); weave(weavers[0]); context->Flush(); NoFrameCheck(bus, swap.Get(), sequence);
    Check(start(nullptr) == ERROR_SUCCESS, "modern11 restart"); NoFrameCheck(bus, swap.Get(), sequence);
    update(204, 34); weave(weavers[0]); context->Flush(); FrameCheck(bus, swap.Get(), sequence, 204, 34); sequence = Latest(bus);
    Check(Counts(runtime) == initialCounts, "modern11 attach/restart replayed setters or constructed a new weaver");
    CheckIdentity(weavers[0], pimpl); weavers[0]->destroy();
    // These objects were also fully configured before attachment. A null input,
    // null context or explicit deferred context must remain uncapturable. Only
    // the following explicit update cases intentionally call a setter again.
    for (size_t i = 1; i < weavers.size(); ++i) {
        weave(weavers[i]); context->Flush(); NoFrameCheck(bus, swap.Get(), sequence);
        if (i == 1) weavers[i]->setInputViewTexture(srv.Get(), 4, 4, desc.Format);
        else weavers[i]->setContext(context.Get());
        const auto r = static_cast<uint8_t>(210 + i), b = static_cast<uint8_t>(40 + i);
        update(r, b); weave(weavers[i]); context->Flush(); FrameCheck(bus, swap.Get(), sequence, r, b); sequence = Latest(bus);
        weavers[i]->destroy();
    }
    // Re-allocation after destruction must not inherit the old object's input.
    SR::IDX11Weaver1* replacement{}; Check(create(nullptr, context.Get(), hwnd, &replacement) == WeaverSuccess, "modern11 replacement");
    weave(replacement); context->Flush(); NoFrameCheck(bus, swap.Get(), sequence);
    replacement->setInputViewTexture(srv.Get(), 4, 4, desc.Format); update(220, 50); weave(replacement); context->Flush(); FrameCheck(bus, swap.Get(), sequence, 220, 50); replacement->destroy();
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == expectedCalls, "modern11 original weave forwarding count");
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaDestroyed")() == 5, "modern11 destroy forwarding count");
    ComPtr<ID3D11InfoQueue> debug; Hr(device.As(&debug), "modern11 debug");
    for (uint64_t i = 0; i < debug->GetNumStoredMessages(); ++i) { SIZE_T size{}; debug->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size); auto m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); debug->GetMessage(i, m, &size); if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription); }
}
void ModernLate12(HMODULE runtime, lan3d::FrameBus& bus, HWND hwnd, Call& stop, Call& start, const wchar_t* capture, bool pimpl) {
    const auto create = Resolve<WeaverErrorCode(*)(SR::SRContext*, ID3D12Device*, HWND, SR::IDX12Weaver1**)>(runtime, Create12);
    Resolve<void(*)(unsigned)>(runtime, "FakeLeiaLayout")(pimpl ? 1 : 0);
    ComPtr<ID3D12Debug> debug; Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "modern12 debug"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "modern12 factory"); ComPtr<IDXGIAdapter> adapter; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "modern12 WARP");
    ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "modern12 device");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "modern12 queue"); auto swap = Swap(factory.Get(), queue.Get(), hwnd);
    ComPtr<ID3D12CommandAllocator> allocator; Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "modern12 allocator");
    ComPtr<ID3D12GraphicsCommandList> list; Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "modern12 list");
    ComPtr<ID3D12CommandAllocator> bundleAllocator; Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAllocator)), "modern12 bundle allocator");
    ComPtr<ID3D12GraphicsCommandList> bundle; Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator.Get(), nullptr, IID_PPV_ARGS(&bundle)), "modern12 bundle");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 8; desc.Height = 4; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; ComPtr<ID3D12Resource> input;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&input)), "modern12 input");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t bytes{}; device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD; ComPtr<ID3D12Resource> upload; Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "modern12 upload");
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "modern12 fence"); uint64_t fenceValue{};
    auto waitGpu = [&] {
        Hr(queue->Signal(fence.Get(), ++fenceValue), "modern12 signal");
        for (unsigned i = 0; i < 2000 && fence->GetCompletedValue() < fenceValue; ++i) Sleep(1);
        Check(fence->GetCompletedValue() == fenceValue, "modern12 GPU timeout");
    };
    auto submit = [&] {
        Hr(list->Close(), "modern12 close"); ID3D12CommandList* submitted[]{list.Get()}; queue->ExecuteCommandLists(1, submitted); waitGpu();
    };
    auto begin = [&] { Hr(allocator->Reset(), "modern12 allocator reset"); Hr(list->Reset(allocator.Get(), nullptr), "modern12 list reset"); };
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    auto transition = [&](D3D12_RESOURCE_STATES next) {
        if (state == next) return;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {input.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, next}; list->ResourceBarrier(1, &barrier); state = next;
    };
    auto update = [&](uint8_t r, uint8_t b) {
        void* data{}; Hr(upload->Map(0, nullptr, &data), "modern12 map"); const auto pixels = Pixels(r, b);
        for (unsigned y = 0; y < 4; ++y) memcpy(static_cast<uint8_t*>(data) + y * footprint.Footprint.RowPitch, pixels.data() + y * 32, 32);
        upload->Unmap(0, nullptr); transition(D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        dst.pResource = input.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    std::array<SR::IDX12Weaver1*, 4> weavers{}; unsigned expectedCalls{};
    auto weave = [&](SR::IDX12Weaver1* w) { w->weave(); ++expectedCalls; };
    update(11, 13);
    for (size_t i = 0; i < weavers.size(); ++i) {
        Check(create(nullptr, device.Get(), hwnd, &weavers[i]) == WeaverSuccess, "modern12 pre-attach factory"); CheckIdentity(weavers[i], pimpl);
        weavers[i]->setInputViewTexture(i == 1 ? nullptr : input.Get(), 4, 4, desc.Format);
        weavers[i]->setCommandList(i == 2 ? nullptr : i == 3 ? bundle.Get() : list.Get());
        weavers[i]->setViewport({0, 0, 8, 4, 0, 1}); weavers[i]->setScissorRect({0, 0, 8, 4}); weave(weavers[i]);
    }
    submit(); swap->Present(0, 0); const auto initialCounts = Counts(runtime);
    // Present and Present1 have separate lazy DXGI setup. Warm the fixture's
    // real presentation path before measuring capture-created host objects;
    // its first Present1 otherwise allocates an internal command allocator.
    DXGI_PRESENT_PARAMETERS presentParameters{};
    for (unsigned i = 0; i < 3; ++i) {
        Hr(swap->Present1(0, 0, &presentParameters), "modern12 pre-attach Present1 warmup");
        waitGpu();
    }
    ObserveHostGraphics(factory.Get(), device.Get());
    StartCapture(capture, bus, stop, start); NoFrameCheck(bus, swap.Get());
    // Discover the pre-existing list from the old weaver. With no newly
    // observed resource barrier it is unsafe to copy, even though input exists.
    begin(); weave(weavers[0]); submit(); NoFrameCheck(bus, swap.Get());
    begin(); update(201, 31); weave(weavers[0]); submit(); FrameCheck(bus, swap.Get(), 0, 201, 31); auto sequence = Latest(bus);
    begin(); update(202, 32); weave(weavers[0]); submit(); FrameCheck(bus, swap.Get(), sequence, 202, 32); sequence = Latest(bus);
    // Reset must clear recorded resource states. Retaining last frame's state
    // would incorrectly make this recording capturable.
    begin(); weave(weavers[0]); submit(); NoFrameCheck(bus, swap.Get(), sequence);
    // Stop while a captured copy is recorded but not submitted. Execute the
    // same recording after Start: the allocation must survive, its frame must
    // be discarded, and neither setter may be called to recover fresh frames.
    begin(); update(44, 45); weave(weavers[0]);
    Check(stop(nullptr) == ERROR_SUCCESS, "modern12 stop"); Check(start(nullptr) == ERROR_SUCCESS, "modern12 restart");
    submit(); NoFrameCheck(bus, swap.Get(), sequence);
    begin(); update(203, 33); weave(weavers[0]); submit(); FrameCheck(bus, swap.Get(), sequence, 203, 33); sequence = Latest(bus);
    Check(Counts(runtime) == initialCounts, "modern12 attach/restart replayed setters or constructed a new weaver");
    CheckIdentity(weavers[0], pimpl); weavers[0]->destroy();
    // Explicit updates below are recovery tests only; every initial hot-attach
    // and stop/restart assertion above ran without a single repeated setter.
    for (size_t i = 1; i < weavers.size(); ++i) {
        begin(); update(210, 40); weave(weavers[i]); submit(); NoFrameCheck(bus, swap.Get(), sequence);
        if (i == 1) weavers[i]->setInputViewTexture(input.Get(), 4, 4, desc.Format);
        else weavers[i]->setCommandList(list.Get());
        const auto r = static_cast<uint8_t>(210 + i), b = static_cast<uint8_t>(40 + i);
        begin(); update(r, b); weave(weavers[i]); submit(); FrameCheck(bus, swap.Get(), sequence, r, b); sequence = Latest(bus); weavers[i]->destroy();
    }
    SR::IDX12Weaver1* replacement{}; Check(create(nullptr, device.Get(), hwnd, &replacement) == WeaverSuccess, "modern12 replacement");
    replacement->setCommandList(list.Get()); begin(); update(220, 50); weave(replacement); submit(); NoFrameCheck(bus, swap.Get(), sequence);
    replacement->setInputViewTexture(input.Get(), 4, 4, desc.Format); begin(); update(221, 51); weave(replacement); submit(); FrameCheck(bus, swap.Get(), sequence, 221, 51); replacement->destroy();
    // FrameCheck also submits Present work. Drain that last presentation before
    // normal local COM destruction releases the swapchain's back buffers.
    waitGpu(); Hr(bundle->Close(), "modern12 bundle close");
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaCalls")() == expectedCalls, "modern12 original weave forwarding count");
    Check(Resolve<unsigned(*)()>(runtime, "FakeLeiaDestroyed")() == 5, "modern12 destroy forwarding count");
    ComPtr<ID3D12InfoQueue> info; Hr(device.As(&info), "modern12 info");
    const auto messages = info->GetNumStoredMessages();
    for (uint64_t i = 0; i < messages; ++i) {
        SIZE_T size{}; Hr(info->GetMessage(i, nullptr, &size), "modern12 debug message size"); std::vector<uint8_t> text(size); auto m = reinterpret_cast<D3D12_MESSAGE*>(text.data());
        Hr(info->GetMessage(i, m, &size), "modern12 debug message");
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << "D3D12 debug error: " << m->pDescription << std::endl; throw std::runtime_error(m->pDescription); }
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 4, "usage: CaptureHooksTests <capture DLL> <SimulatedRealityDirectX.dll> <dx11|dx12|legacy11|legacy12|late11|late12|late11-input|late12-input|modern-late11|modern-late12|modern-late11-pimpl|modern-late12-pimpl>");
        std::wstring mode = argv[3];
        const bool modern11 = mode == L"modern-late11" || mode == L"modern-late11-pimpl";
        const bool modern12 = mode == L"modern-late12" || mode == L"modern-late12-pimpl";
        const bool modern = modern11 || modern12, pimpl = mode.ends_with(L"-pimpl");
        bool late = mode == L"late11" || mode == L"late12" || mode == L"late11-input" || mode == L"late12-input";
        bool dx12 = mode == L"dx12" || mode == L"legacy12" || mode == L"late12" || mode == L"late12-input";
        bool legacy = late || mode == L"legacy11" || mode == L"legacy12";
        bool inputMode = mode == L"late11-input" || mode == L"late12-input";
        Check(modern || late || mode == L"dx11" || mode == L"dx12" || mode == L"legacy11" || mode == L"legacy12", "unknown test mode");
        // Probe in this clean test helper before fixture objects, third-party
        // observers, fake Leia runtime, or capture hooks exist. The profile is
        // cached across Start/Stop; capture may never probe inside its target.
        std::string probeError;
        if (!lan3d::ProbeNativeGpuHooks(nativeGpuHooks, probeError)) throw std::runtime_error("native GPU probe: " + probeError);
        Call start{}, stop{}; lan3d::FrameBus bus;
        if (!late && !modern) StartCapture(argv[1], bus, stop, start);
        // Normal modes load the runtime after capture started. Late modes load
        // capture only after the runtime, weaver and GPU resources already exist.
        // Forwarded exports reside in FakeLeiaRuntime.dll, not this module.
        auto runtime = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); Check(runtime != nullptr, "fake runtime load");
        if (!late && !modern) WaitForRuntimeHooks(bus);
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LAN3DCaptureTest"; RegisterClassW(&wc);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr); Check(window != nullptr, "window");
        if (modern11) ModernLate11(runtime, bus, window, stop, start, argv[1], pimpl);
        else if (modern12) ModernLate12(runtime, bus, window, stop, start, argv[1], pimpl);
        else if (dx12) Test12(runtime, bus, window, legacy, stop, start, late ? argv[1] : nullptr, inputMode);
        else Test11(runtime, bus, window, legacy, stop, start, late ? argv[1] : nullptr, inputMode);
        stop(nullptr); CheckHostGraphicsUnchanged(); DestroyWindow(window); std::cout << "factory/export hooks, late attach, actual WARP GPU path, Present1 selection, forward/query/destroy semantics PASS\n";
        if (modern) std::cout << "host device/factory/queue/allocator/list/swapchain creation count remains zero during hot attach and restart PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
