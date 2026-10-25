#include "NativeGpuHooks.h"
#include "GpuSlots.h"
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cwchar>
#include <sstream>
#include <stdexcept>

namespace lan3d {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned MaxAdapters = 16;

class ProbeWindow {
    HINSTANCE instance_{GetModuleHandleW(nullptr)};
    std::wstring className_;
    ATOM atom_{};
public:
    HWND handle{};
    ProbeWindow() {
        static std::atomic<unsigned> serial{};
        className_ = L"LAN3DNativeGpuProbe." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(++serial);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance_;
        wc.lpszClassName = className_.c_str();
        atom_ = RegisterClassExW(&wc);
        if (atom_) handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className_.c_str(), L"",
            WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, instance_, nullptr);
    }
    ~ProbeWindow() {
        // The window belongs to this probe thread, including every DXGI call
        // and final swap-chain Release. It is never shown or moved to a worker.
        if (handle) DestroyWindow(handle);
        if (atom_) UnregisterClassW(className_.c_str(), instance_);
    }
    ProbeWindow(const ProbeWindow&) = delete;
    ProbeWindow& operator=(const ProbeWindow&) = delete;
};

struct Probe {
    NativeGpuHooks& hooks;
    std::ostringstream failures;
    unsigned adapters{}, devices11{}, devices12{}, swaps{}, queues{}, debugQueues{};

    void Add(void* address, NativeGpuHook kind) {
        HMODULE module{};
        if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(address), &module)) throw std::runtime_error("GPU entry has no loaded PE module");
        struct ModuleRef { HMODULE value; ~ModuleRef() { FreeLibrary(value); } } reference{module};
        NativeGpuModule identity{};
        if (!ReadNativeGpuModule(module, identity)) throw std::runtime_error("cannot identify GPU entry module");
        const auto rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
        MEMORY_BASIC_INFORMATION memory{};
        if (rva >= identity.imageSize || !VirtualQuery(address, &memory, sizeof(memory)) ||
            memory.AllocationBase != module || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            throw std::runtime_error("GPU entry is outside its executable image");
        uint32_t index{};
        for (; index < hooks.moduleCount; ++index) {
            const auto& known = hooks.modules[index];
            if (_wcsicmp(known.path, identity.path)) continue;
            if (known.timestamp != identity.timestamp || known.imageSize != identity.imageSize ||
                !IsEqualGUID(known.pdbGuid, identity.pdbGuid) || known.pdbAge != identity.pdbAge)
                throw std::runtime_error("GPU module identity changed during probe");
            break;
        }
        if (index == hooks.moduleCount) {
            if (index == hooks.modules.size()) throw std::runtime_error("GPU probe module capacity exceeded");
            hooks.modules[hooks.moduleCount++] = identity;
        }
        for (uint32_t i = 0; i < hooks.siteCount; ++i) {
            const auto& known = hooks.sites[i];
            if (known.kind == kind && known.module == index && known.rva == rva) return;
        }
        if (hooks.siteCount == hooks.sites.size()) throw std::runtime_error("GPU probe entry capacity exceeded");
        hooks.sites[hooks.siteCount++] = {kind, index, static_cast<uint32_t>(rva), 0};
    }
    void Failure(const std::string& path, HRESULT hr) {
        failures << path << "=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "; ";
    }
    void Swap(IDXGIFactory2* factory, IUnknown* device, const std::string& label) {
        ProbeWindow window;
        if (!window.handle) { Failure(label, HRESULT_FROM_WIN32(GetLastError())); return; }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 16;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Declared after the HWND so all interfaces die before DestroyWindow.
        ComPtr<IDXGISwapChain1> swap;
        const auto hr = factory->CreateSwapChainForHwnd(device, window.handle, &desc, nullptr, nullptr, &swap);
        if (FAILED(hr)) { Failure(label, hr); return; }
        Add((*reinterpret_cast<void***>(swap.Get()))[CaptureGetGpuSlot(SlotPresent)], NativeGpuHook::Present);
        Add((*reinterpret_cast<void***>(swap.Get()))[CaptureGetGpuSlot(SlotPresent1)], NativeGpuHook::Present1);
        ++swaps;
    }
    void Adapter(IDXGIFactory2* factory, IDXGIAdapter1* adapter, bool warp, bool debug) {
        if (!debug) ++adapters;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter->GetDesc1(&adapterDesc))) return;
        std::ostringstream name;
        name << (warp ? "WARP" : "hardware") << '[' << std::hex <<
            static_cast<uint32_t>(adapterDesc.AdapterLuid.HighPart) << ':' << adapterDesc.AdapterLuid.LowPart << ']';
        if (debug) name << " debug";
        const auto label = name.str();
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (!debug) {
            ComPtr<ID3D11Device> device11;
            ComPtr<ID3D11DeviceContext> context;
            const auto hr11 = D3D11CreateDevice(warp ? nullptr : adapter,
                warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                D3D11_SDK_VERSION, &device11, nullptr, &context);
            if (SUCCEEDED(hr11)) { ++devices11; Swap(factory, device11.Get(), label + " DX11 swap"); }
            else Failure(label + " DX11", hr11);
        }

        ComPtr<ID3D12Device> device12;
        const auto hr12 = D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&device12));
        if (FAILED(hr12)) { Failure(label + " DX12", hr12); return; }
        ++devices12;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        const auto queueResult = device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        if (FAILED(queueResult)) { Failure(label + " queue", queueResult); return; }
        Add((*reinterpret_cast<void***>(queue.Get()))[CaptureGetGpuSlot(SlotQueueExecute)], NativeGpuHook::QueueExecute);
        // Release is the shared public IUnknown slot, derived from SDK headers.
        Add((*reinterpret_cast<void***>(queue.Get()))[CaptureGetGpuSlot(SlotListRelease)], NativeGpuHook::QueueRelease);
        ++queues;
        if (debug) ++debugQueues;
        Swap(factory, queue.Get(), label + " DX12 swap");
        // No command list, GPU submission, hook or process injection is needed.
    }
    void Enumerate(IDXGIFactory4* factory, bool debug) {
        for (unsigned i = 0; i <= MaxAdapters; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) { Failure("adapter enumeration", hr); break; }
            if (i == MaxAdapters) throw std::runtime_error("GPU probe adapter capacity exceeded");
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) continue;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) Adapter(factory, adapter.Get(), false, debug);
        }
        ComPtr<IDXGIAdapter1> warp;
        const auto warpResult = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        if (SUCCEEDED(warpResult)) Adapter(factory, warp.Get(), true, debug);
        else Failure("WARP enumeration", warpResult);
    }
};
}

bool ProbeNativeGpuHooks(NativeGpuHooks& hooks, std::string& error) {
    hooks = {};
    error.clear();
    try {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            error = "native GPU probe could not create a DXGI factory"; return false;
        }
        Probe probe{hooks};
        probe.Enumerate(factory.Get(), false);
        const bool normalReady = probe.swaps && probe.queues;
        {
            // The caller must run this helper before initializing its own DX12
            // devices. All normal probe devices are already destroyed here.
            // Debug wrappers use different queue/list identities, so include
            // their entrypoints as an optional, separate probe pass.
            ComPtr<ID3D12Debug4> debug;
            const auto debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
            if (SUCCEEDED(debugResult)) {
                struct DebugScope {
                    ID3D12Debug4* debug;
                    ~DebugScope() { debug->DisableDebugLayer(); }
                } disable{debug.Get()};
                debug->EnableDebugLayer();
                probe.Enumerate(factory.Get(), true);
            } else probe.Failure("optional DX12 debug layer", debugResult);
        }
        std::ostringstream diagnostic;
        diagnostic << "native GPU probe: adapters=" << probe.adapters << " DX11=" << probe.devices11 <<
            " DX12=" << probe.devices12 << " swapchains=" << probe.swaps << " queues=" << probe.queues <<
            " debugQueues=" << probe.debugQueues <<
            " modules=" << hooks.moduleCount << " entries=" << hooks.siteCount;
        const auto failures = probe.failures.str();
        if (!failures.empty()) diagnostic << "; unavailable: " << failures;
        error = diagnostic.str();
        if (normalReady) return true;
        error += "; both a Present path and a DX12 queue path are required";
    } catch (const std::exception& exception) { error = std::string("native GPU probe failed: ") + exception.what(); }
    catch (...) { error = "native GPU probe failed with an unexpected exception"; }
    hooks = {};
    return false;
}
}
