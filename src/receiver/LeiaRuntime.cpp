#include "LeiaRuntime.h"
#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace lan3d::receiver {
namespace {
template<class T> T Resolve(HMODULE module, const char* name, bool required = true) {
    auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result && required) throw std::runtime_error(std::string("LeiaSR runtime export is missing: ") + name);
    return result;
}
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}
LeiaRuntime::~LeiaRuntime() { Close(); }

void LeiaRuntime::Open(const std::filesystem::path& directory) {
    directory_ = std::filesystem::canonical(directory);
    // Resolve the complete selected runtime from one directory. Never fall back
    // to the executable directory, CWD, PATH, or a different installed Leia SDK.
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        throw std::runtime_error("Cannot restrict the receiver DLL search path");
    directoryCookie_ = AddDllDirectory(directory_.c_str());
    if (!directoryCookie_) throw std::runtime_error("Cannot add the selected LeiaSR runtime directory");
    ValidateModuleOrigins();
    auto load = [&](const wchar_t* name) {
        auto path = directory_ / name;
        auto module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!module) throw std::runtime_error("Cannot load " + PathText(path) + " (Windows error " + std::to_string(GetLastError()) + ")");
        return module;
    };
    core_ = load(L"SimulatedRealityCore.dll");
    directx_ = load(L"SimulatedRealityDirectX.dll");
    using Create = SR::SRContext* (*)(SR::SRContext::NetworkMode);
    auto create = Resolve<Create>(core_, "?create@SRContext@SR@@SAPEAV12@W4NetworkMode@12@@Z");
    initialize_ = Resolve<Initialize>(core_, "?initialize@SRContext@SR@@QEAAXXZ");
    deleteContext_ = Resolve<DeleteContext>(core_, "?deleteSRContext@SRContext@SR@@SAXPEAV12@@Z");
    ValidateModuleOrigins();
    context_ = create(SR::SRContext::NonBlockingClientMode);
    if (!context_) throw std::runtime_error("LeiaSR returned a null context; verify the selected runtime service is running");
    ValidateModuleOrigins();
    std::cout << "Runtime: " << PathText(directory_) << "\n";
}

void LeiaRuntime::ValidateModuleOrigins() {
    std::vector<HMODULE> modules(1024);
    DWORD needed{};
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed))
        throw std::runtime_error("Cannot inspect loaded LeiaSR runtime modules");
    if (needed > modules.size() * sizeof(HMODULE)) {
        modules.resize(needed / sizeof(HMODULE));
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), needed, &needed))
            throw std::runtime_error("Cannot inspect loaded LeiaSR runtime modules");
    }
    modules.resize(needed / sizeof(HMODULE));
    for (auto module : modules) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(module, path, DWORD(std::size(path)))) continue;
        const auto name = Lower(std::filesystem::path(path).filename().wstring());
        if (name.rfind(L"simulatedreality", 0) != 0 && name != L"titanleiaruntime.dll") continue;
        if (Lower(std::filesystem::weakly_canonical(std::filesystem::path(path).parent_path()).wstring()) != Lower(directory_.wstring()))
            throw std::runtime_error("Mixed LeiaSR runtime detected: " + PathText(path));
    }
}

void LeiaRuntime::SetInput(ID3D11Device* device, ID3D11DeviceContext* dc, HWND window,
                          ID3D11ShaderResourceView* input, unsigned packedWidth, unsigned height) {
    if (!context_ || !input || packedWidth < 4 || packedWidth % 2) throw std::runtime_error("Invalid LeiaSR SBS input");
    if (legacyConstructed_ && (width_ != packedWidth || height_ != height)) DestroyWeaver();
    if (!modern_ && !legacyConstructed_) {
        using Create = WeaverErrorCode (*)(SR::SRContext*, ID3D11DeviceContext*, HWND, SR::IDX11Weaver1**);
        auto create = Resolve<Create>(directx_, "?CreateDX11Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D11DeviceContext@@PEAUHWND__@@PEAPEAVIDX11Weaver1@1@@Z", false);
        if (create && !modernUnavailable_) {
            const auto result = create(context_, dc, window, &modern_);
            if (result != WeaverSuccess || !modern_) {
                if (modern_) { modern_->destroy(); modern_ = nullptr; }
                modernUnavailable_ = true;
                std::cout << "New DX11 weaver unavailable (code " << int(result) << "); trying legacy DX11 ABI\n";
            }
        }
        if (!modern_) {
            legacyCtor_ = Resolve<LegacyCtor>(directx_, "??0PredictingDX11Weaver@SR@@QEAA@AEAVSRContext@1@PEAUID3D11Device@@PEAUID3D11DeviceContext@@IIW4DXGI_FORMAT@@PEAUHWND__@@@Z");
            legacyDtor_ = Resolve<LegacyDtor>(directx_, "??1PredictingDX11Weaver@SR@@QEAA@XZ");
            legacyInput_ = Resolve<LegacyInput>(directx_, "?setInputFrameBuffer@DX11WeaverBase@SR@@QEAAXPEAUID3D11ShaderResourceView@@@Z");
            legacyWeave_ = Resolve<LegacyWeave>(directx_, "?weave@DX11WeaverBase@SR@@QEAAXII@Z");
            legacyCanWeave_ = Resolve<LegacyCanWeave>(directx_, "?canWeave@DX11WeaverBase@SR@@QEAA_NXZ", false);
            legacyLatency_ = Resolve<LegacyLatency>(directx_, "?setLatencyInFrames@PredictingDX11Weaver@SR@@QEAAX_K@Z", false);
            ValidateModuleOrigins();
            // Legacy constructors take the complete packed texture width.
            legacyCtor_(legacy_, context_, device, dc, packedWidth, height, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, window);
            legacyConstructed_ = true;
            if (legacyLatency_) legacyLatency_(legacy_, 1);
        } else {
            modern_->setShaderSRGBConversion(false, false); // Hardware sRGB SRV + RTV.
            modern_->setLatencyInFrames(1);
            modern_->enableLateLatching(true);
        }
    }
    if (modern_) {
        // The official DX11 SDK sample supplies one-eye width here, despite the
        // header's generic "texture width" wording. SRV holds both eye images.
        modern_->setInputViewTexture(input, int(packedWidth / 2), int(height), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    } else legacyInput_(legacy_, input);
    width_ = packedWidth;
    height_ = height;
    if (!initialized_) { initialize_(context_); initialized_ = true; }
    ValidateModuleOrigins();
}

bool LeiaRuntime::Weave(unsigned width, unsigned height) {
    if (modern_) { modern_->weave(); return true; }
    if (!legacyConstructed_) return false;
    if (legacyCanWeave_ && !legacyCanWeave_(legacy_)) return false;
    legacyWeave_(legacy_, width, height);
    return true;
}
void LeiaRuntime::DestroyWeaver() noexcept {
    try {
        if (modern_) { modern_->destroy(); modern_ = nullptr; }
        if (legacyConstructed_) { legacyDtor_(legacy_); legacyConstructed_ = false; }
    } catch (...) { /* Destructors cannot report a second exception. */ }
    width_ = height_ = 0;
}
void LeiaRuntime::Close() noexcept {
    DestroyWeaver();
    try { if (context_ && deleteContext_) deleteContext_(context_); } catch (...) {}
    context_ = nullptr;
    if (directx_) { FreeLibrary(directx_); directx_ = nullptr; }
    if (core_) { FreeLibrary(core_); core_ = nullptr; }
    if (directoryCookie_) { RemoveDllDirectory(directoryCookie_); directoryCookie_ = nullptr; }
    initialized_ = false;
    modernUnavailable_ = false;
}
}
