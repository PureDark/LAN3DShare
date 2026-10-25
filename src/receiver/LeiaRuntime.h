#pragma once
#include <windows.h>
#include <d3d11.h>
#include <filesystem>
#include <string>
#include <sr/weaver/dx11weaver.h>

namespace lan3d::receiver {
// Uses the SDK's actual class layout, including its alignment. No local vtable
// replicas, runtime import libraries, or guessed legacy object sizes are used.
class LeiaRuntime {
public:
    ~LeiaRuntime();
    void Open(const std::filesystem::path& directory);
    void SetInput(ID3D11Device*, ID3D11DeviceContext*, HWND,
                  ID3D11ShaderResourceView*, unsigned packedWidth, unsigned height);
    bool Weave(unsigned width, unsigned height);
    void DestroyWeaver() noexcept;
    void Close() noexcept;
    const char* Mode() const { return modern_ ? "LeiaSR DX11" : "LeiaSR legacy DX11"; }
private:
    void ValidateModuleOrigins();
    HMODULE core_{}, directx_{};
    DLL_DIRECTORY_COOKIE directoryCookie_{};
    std::filesystem::path directory_;
    SR::SRContext* context_{};
    SR::IDX11Weaver1* modern_{};
    alignas(SR::PredictingDX11Weaver) unsigned char legacy_[sizeof(SR::PredictingDX11Weaver)]{};
    bool legacyConstructed_{}, initialized_{}, modernUnavailable_{};
    unsigned width_{}, height_{};
    using DeleteContext = void (*)(SR::SRContext*);
    using Initialize = void (*)(SR::SRContext*);
    using LegacyCtor = void (*)(void*, SR::SRContext*, ID3D11Device*, ID3D11DeviceContext*, unsigned, unsigned, DXGI_FORMAT, HWND);
    using LegacyDtor = void (*)(void*);
    using LegacyInput = void (*)(void*, ID3D11ShaderResourceView*);
    using LegacyWeave = void (*)(void*, unsigned, unsigned);
    using LegacyCanWeave = bool (*)(void*);
    using LegacyLatency = void (*)(void*, uint64_t);
    DeleteContext deleteContext_{};
    Initialize initialize_{};
    LegacyCtor legacyCtor_{};
    LegacyDtor legacyDtor_{};
    LegacyInput legacyInput_{};
    LegacyWeave legacyWeave_{};
    LegacyCanWeave legacyCanWeave_{};
    LegacyLatency legacyLatency_{};
};
}
