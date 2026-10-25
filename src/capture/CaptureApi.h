#pragma once
#include <Windows.h>
#include <cstdint>

namespace lan3d {
inline constexpr uint32_t CaptureGpuOutput = 1;
// Fixed-width remote-call payload. Configure may be called before or after Start.
// Zero stream selects the first observed weaver. Zero ordinal selects the last
// weave preceding the selected window's Present; otherwise ordinal is one-based.
struct CaptureConfig {
    uint32_t size{sizeof(CaptureConfig)};
    uint32_t version{1};
    uint32_t streamId{};
    uint32_t weaveOrdinal{};
    uint64_t window{};
    uint32_t maxFps{60};
    uint32_t reserved{}; // CaptureGpuOutput requests the shared-texture path.
};
static_assert(sizeof(CaptureConfig) == 32);
}

extern "C" {
// All return Win32 error codes. A stopped capture DLL stays loaded because
// runtime objects / command lists can retain hooked code until process exit.
#ifdef LAN3D_CAPTURE_EXPORTS
#define LAN3D_CAPTURE_API __declspec(dllexport)
#else
#define LAN3D_CAPTURE_API
#endif
LAN3D_CAPTURE_API DWORD WINAPI LAN3DConfigure(void* config);
// NativeGpuHooks is produced outside the target process. Configure this before
// Start; an active capture rejects profile replacement.
LAN3D_CAPTURE_API DWORD WINAPI LAN3DSetGpuHooks(void* hooks);
LAN3D_CAPTURE_API DWORD WINAPI LAN3DStart(void* unused);
LAN3D_CAPTURE_API DWORD WINAPI LAN3DStop(void* unused);
#undef LAN3D_CAPTURE_API
}
