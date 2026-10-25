#pragma once
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <algorithm>

namespace lan3d::media {
struct GpuAdapter { std::string id; std::wstring name; LUID luid{}; unsigned index{}; };
inline bool SameAdapter(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
inline std::vector<GpuAdapter> EncodeAdapters() {
    // DXGI may expose virtual/indirect adapters with the same NVIDIA name.
    // Match physical CUDA device LUIDs, and use NVIDIA's device numbering.
    static const auto cached = [] {
    std::vector<LUID> physical;
    std::vector<std::string> identifiers;
    const auto cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cuda) {
        const auto init = reinterpret_cast<int(WINAPI*)(unsigned)>(GetProcAddress(cuda, "cuInit"));
        const auto count = reinterpret_cast<int(WINAPI*)(int*)>(GetProcAddress(cuda, "cuDeviceGetCount"));
        const auto luid = reinterpret_cast<int(WINAPI*)(char*, unsigned*, int)>(GetProcAddress(cuda, "cuDeviceGetLuid"));
        const auto pci = reinterpret_cast<int(WINAPI*)(char*, int, int)>(GetProcAddress(cuda, "cuDeviceGetPCIBusId"));
        int n{};
        if (init && count && luid && init(0) == 0 && count(&n) == 0) for (int i = 0; i < n; ++i) {
            LUID id{}; unsigned mask{};
            if (luid(reinterpret_cast<char*>(&id), &mask, i) == 0) {
                physical.push_back(id); char bus[32]{};
                identifiers.push_back(pci && pci(bus, sizeof(bus), i) == 0 ? "pci:" + std::string(bus) : "cuda:" + std::to_string(i));
            }
        }
        FreeLibrary(cuda);
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    std::vector<GpuAdapter> result;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    for (unsigned i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapter || FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        auto found = std::find_if(physical.begin(), physical.end(), [&](LUID id) { return SameAdapter(id, desc.AdapterLuid); });
        if (found == physical.end()) continue;
        result.push_back({identifiers[size_t(found - physical.begin())], desc.Description, desc.AdapterLuid, unsigned(found - physical.begin())});
    }
    return result;
    }();
    return cached;
}
}
