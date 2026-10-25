#define CINTERFACE
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stddef.h>
// Keep the switch order aligned with CaptureGpuSlot in GpuSlots.h.
unsigned CaptureGetGpuSlot(int slot) {
#define SLOT(T, M) ((unsigned)(offsetof(T, M) / sizeof(void*)))
    switch (slot) {
    case 0: return SLOT(IDXGISwapChainVtbl, Present);
    case 1: return SLOT(IDXGISwapChain1Vtbl, Present1);
    case 2: return SLOT(ID3D12GraphicsCommandListVtbl, Reset);
    case 3: return SLOT(ID3D12GraphicsCommandListVtbl, Release);
    case 4: return SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier);
    case 5: return SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier);
    case 6: return SLOT(ID3D12GraphicsCommandListVtbl, ExecuteBundle);
    case 7: return SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);
    case 8: return SLOT(ID3D12DeviceVtbl, CreateCommandQueue);
    case 9: return SLOT(ID3D12DeviceVtbl, CreateCommandList);
    case 10: return SLOT(ID3D12Device4Vtbl, CreateCommandList1);
    case 11: return SLOT(IDXGIFactoryVtbl, CreateSwapChain);
    case 12: return SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);
    case 13: return SLOT(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow);
    case 14: return SLOT(IDXGIFactory2Vtbl, CreateSwapChainForComposition);
    default: return ~0u;
    }
#undef SLOT
}
