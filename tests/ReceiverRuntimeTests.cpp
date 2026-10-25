#include "receiver/LeiaRuntime.h"
#include "receiver_runtime/TestState.h"
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 3, "Expected test runtime directory and modern/legacy mode");
        const bool legacy = !wcscmp(argv[2], L"legacy");
        SetEnvironmentVariableW(L"LAN3D_RECEIVER_TEST_LEGACY", legacy ? L"1" : nullptr);
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device creation failed");
        lan3d::receiver::LeiaRuntime runtime;
        runtime.Open(argv[1]);
        // Retain explicit references only so counters remain inspectable after
        // Runtime.Close; both modules still come from the selected test folder.
        HMODULE core = LoadLibraryW((std::filesystem::path(argv[1]) / L"SimulatedRealityCore.dll").c_str());
        HMODULE directx = LoadLibraryW((std::filesystem::path(argv[1]) / L"SimulatedRealityDirectX.dll").c_str());
        auto getCore = reinterpret_cast<ReceiverCoreState (*)()>(GetProcAddress(core, "ReceiverGetCoreState"));
        auto getWeaver = reinterpret_cast<ReceiverWeaverState (*)()>(GetProcAddress(directx, "ReceiverGetWeaverState"));
        Require(getCore && getWeaver, "Test module counter exports missing");
        for (unsigned width : {640u, 800u}) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=360;
            desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            Require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)), "Texture creation failed");
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
            ComPtr<ID3D11ShaderResourceView> srv;
            Require(SUCCEEDED(device->CreateShaderResourceView(texture.Get(),&view,&srv)), "SRV creation failed");
            runtime.SetInput(device.Get(),context.Get(),nullptr,srv.Get(),width,360);
            Require(runtime.Weave(1280,720), "Test weaver did not execute");
            const auto state=getWeaver();
            Require(state.textureWidth==width && state.suppliedWidth==(legacy?width:width/2), "SBS width contract mismatch");
            Require(state.height==360 && state.latencyFrames==1, "Height/latency contract mismatch");
        }
        runtime.DestroyWeaver();
        context->ClearState(); context->Flush();
        runtime.Close();
        const auto c=getCore(); const auto w=getWeaver();
        Require(c.created==1 && c.initialized==1 && c.destroyed==1, "Context lifecycle mismatch");
        Require(w.inputCount==2 && w.weaveCount==2, "Input/weave call count mismatch");
        if (legacy) Require(w.modernAttempts==1 && w.modernCreated==0 && w.legacyCreated==2 && w.legacyDestroyed==2, "Legacy fallback lifecycle mismatch");
        else Require(w.modernAttempts==1 && w.modernCreated==1 && w.modernDestroyed==1 && w.legacyCreated==0 && !w.srgbRead && !w.srgbWrite, "Modern lifecycle/color mismatch");
        FreeLibrary(directx); FreeLibrary(core);
        std::cout << "Receiver runtime " << (legacy?"legacy":"modern") << " PASS: width contract, resize, local weaver, context lifecycle\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
