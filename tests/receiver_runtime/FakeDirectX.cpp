#define COMPILING_DLL_SimulatedRealityDirectX
#include <sr/weaver/dx11weaver.h>
#include "TestState.h"
#include <wrl/client.h>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
ReceiverWeaverState state;
void Input(ID3D11ShaderResourceView* input, unsigned width, unsigned height) {
    if (!input) throw std::runtime_error("Test weaver got a null SRV");
    ComPtr<ID3D11Resource> resource; input->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) throw std::runtime_error("Test weaver input must be Texture2D");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    ++state.inputCount; state.suppliedWidth = width; state.height = height; state.textureWidth = desc.Width;
}
class Modern final : public SR::IDX11Weaver1 {
public:
    void destroy() override { ++state.modernDestroyed; delete this; }
    void* queryInterface(std::type_index) override { return nullptr; }
    void setWindowHandle(HWND) override {}
    void weave() override { ++state.weaveCount; }
    void enableLateLatching(bool) override {}
    bool isLateLatchingEnabled() const override { return false; }
    void setShaderSRGBConversion(bool r, bool w) override { state.srgbRead = r; state.srgbWrite = w; }
    void setLatencyInFrames(uint64_t value) override { state.latencyFrames = unsigned(value); }
    void setLatency(uint64_t) override {}
    uint64_t getLatency() const override { return 0; }
    void getPredictedEyePositions(float left[3], float right[3]) override { for (int i=0;i<3;++i) left[i]=right[i]=0; }
    void setInputViewTexture(ID3D11ShaderResourceView* input, int w, int h, DXGI_FORMAT format) override {
        if (format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) throw std::runtime_error("Expected hardware sRGB input format");
        Input(input, unsigned(w), unsigned(h));
    }
    void setContext(ID3D11DeviceContext*) override {}
};
}
namespace SR {
IDX11Weaver1::~IDX11Weaver1() = default;
WeaverErrorCode CreateDX11Weaver(SRContext*, ID3D11DeviceContext*, HWND, IDX11Weaver1** output) {
    ++state.modernAttempts;
    if (GetEnvironmentVariableW(L"LAN3D_RECEIVER_TEST_LEGACY", nullptr, 0)) { *output = nullptr; return WeaverTextureFailedToLoad; }
    *output = new Modern; ++state.modernCreated; return WeaverSuccess;
}
class DX11WeaverBase::Impl {
public:
    unsigned width{}, height{};
};
DX11WeaverBase::DX11WeaverBase() = default;
DX11WeaverBase::~DX11WeaverBase() = default;
PredictingDX11Weaver::PredictingDX11Weaver(SRContext&, ID3D11Device*, ID3D11DeviceContext*, unsigned w, unsigned h, DXGI_FORMAT f, HWND) {
    if (f != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) throw std::runtime_error("Expected legacy hardware sRGB format");
    pimpl = std::make_unique<Impl>(); pimpl->width = w; pimpl->height = h; ++state.legacyCreated;
}
PredictingDX11Weaver::~PredictingDX11Weaver() { ++state.legacyDestroyed; }
void PredictingDX11Weaver::setLatencyInFrames(uint64_t value) { state.latencyFrames = unsigned(value); }
void DX11WeaverBase::setInputFrameBuffer(ID3D11ShaderResourceView* input) { Input(input, pimpl->width, pimpl->height); }
void DX11WeaverBase::weave(unsigned, unsigned) { ++state.weaveCount; }
bool DX11WeaverBase::canWeave() { return true; }
}
extern "C" __declspec(dllexport) ReceiverWeaverState ReceiverGetWeaverState() { return state; }
