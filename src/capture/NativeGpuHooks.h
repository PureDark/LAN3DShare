#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>

namespace lan3d {
enum class NativeGpuHook : uint32_t { Present, Present1, QueueExecute, QueueRelease };
struct NativeGpuModule {
    wchar_t path[520]{};
    uint32_t timestamp{}, imageSize{};
    GUID pdbGuid{};
    uint32_t pdbAge{};
};
struct NativeGpuSite { NativeGpuHook kind{}; uint32_t module{}, rva{}, reserved{}; };
struct NativeGpuHooks {
    uint32_t size{sizeof(NativeGpuHooks)}, version{1}, moduleCount{}, siteCount{};
    std::array<NativeGpuModule, 24> modules{};
    std::array<NativeGpuSite, 96> sites{};
};
// Run in the sender/helper process, NEVER inside the capture target.
bool ProbeNativeGpuHooks(NativeGpuHooks& hooks, std::string& error);
// Metadata-only identity check; no device, queue, factory or swap chain creation.
bool ReadNativeGpuModule(HMODULE module, NativeGpuModule& info);
}
