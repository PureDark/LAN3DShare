#include "capture/NativeGpuHooks.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Hr(HRESULT hr, const char* message) { if (FAILED(hr)) throw std::runtime_error(message); }
struct LiveDevice { ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Fence> fence; };
int main(int argc, char** argv) try {
    const bool late = argc > 1 && std::string(argv[1]) == "--reproduce-late";
    lan3d::NativeGpuHooks cached;
    std::string diagnostic;
    if (!late && !lan3d::ProbeNativeGpuHooks(cached,diagnostic)) throw std::runtime_error(diagnostic);
    ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    std::vector<LiveDevice> live;
    for (UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{}; if(!adapter || FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))continue;
        LiveDevice d;
        if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device))))continue;
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        Hr(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue)),"COPY queue");
        Hr(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.fence)),"fence");
        live.push_back(std::move(d));
    }
    if(live.empty())return 77;
    if(late) {
        // Deliberately reproduce the previous sender order, in this disposable
        // process only. Never inject or change another application's devices.
        lan3d::ProbeNativeGpuHooks(cached,diagnostic);
        unsigned reset{};
        for(const auto& d:live){const auto reason=d.device->GetDeviceRemovedReason();if(FAILED(reason))++reset;std::cout<<"LATE_PROBE removed_reason=0x"<<std::hex<<uint32_t(reason)<<std::dec<<'\n';}
        if(!reset){std::cout<<"SKIP: optional debug layer did not invalidate existing devices\n";return 77;}
        std::cout<<"Reproduced late probe invalidation on "<<reset<<" devices\n";return 0;
    }
    if(!cached.moduleCount || !cached.siteCount)throw std::runtime_error("empty startup profile");
    for(uint64_t value=1;value<=10;++value) {
        // Reusing the prepared profile for later attachment leaves active
        // devices and both adapters' copy queues intact.
        const auto attachedProfile=cached;
        if(attachedProfile.siteCount!=cached.siteCount)throw std::runtime_error("profile changed");
        for(const auto& d:live) {
            Hr(d.device->GetDeviceRemovedReason(),"startup probe damaged later device");
            Hr(d.queue->Signal(d.fence.Get(),value),"COPY signal");
            const auto until=GetTickCount64()+3000;
            while(d.fence->GetCompletedValue()<value && GetTickCount64()<until)Sleep(1);
            if(d.fence->GetCompletedValue()!=value)throw std::runtime_error("COPY fence timeout/device loss");
        }
    }
    std::cout<<"STARTUP_PROBE cached=1 healthy_devices="<<live.size()<<" copy_fence_rounds=10\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
