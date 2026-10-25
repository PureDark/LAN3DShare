#pragma once
// Computed from Windows SDK C COM vtables in GpuSlots.c; no handwritten indices.
enum CaptureGpuSlot {
    SlotPresent, SlotPresent1, SlotListReset, SlotListRelease, SlotListBarrier,
    SlotListEnhancedBarrier, SlotListExecuteBundle, SlotQueueExecute,
    SlotDeviceCreateQueue, SlotDeviceCreateList, SlotDeviceCreateList1,
    SlotFactoryCreateSwapChain, SlotFactoryCreateSwapChainForHwnd,
    SlotFactoryCreateSwapChainForCoreWindow, SlotFactoryCreateSwapChainForComposition
};
extern "C" unsigned CaptureGetGpuSlot(CaptureGpuSlot slot);
