#define COMPILING_DLL_SimulatedRealityDirectX
#include <sr/weaver/dx11weaver.h>
#include <sr/weaver/dx12weaver.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<unsigned> calls{}, destroyed{}, inputs{}, contexts{}, lists{}, factories{}, layout{};
template<class Interface> class Fake final : public Interface {
public:
    void destroy() override { ++destroyed; delete this; }
    void setWindowHandle(HWND w) override { window_ = w; }
    void weave() override { ++calls; }
    void enableLateLatching(bool value) override { late_ = value; }
    bool isLateLatchingEnabled() const override { return late_; }
    void setShaderSRGBConversion(bool r, bool w) override { srgb_ = r || w; }
    void setLatencyInFrames(uint64_t value) override { latency_ = value * 16666; }
    void setLatency(uint64_t value) override { latency_ = value; }
    uint64_t getLatency() const override { return latency_; }
    void getPredictedEyePositions(float left[3], float right[3]) override { for (int i = 0; i < 3; ++i) left[i] = right[i] = 100.f + i; }
    void setInputViewTexture(ID3D11ShaderResourceView* v, int, int, DXGI_FORMAT f) { input11_ = v; format_ = f; ++inputs; }
    void setInputViewTexture(ID3D12Resource* v, int, int, DXGI_FORMAT f) { input12_ = v; format_ = f; ++inputs; }
    void setContext(ID3D11DeviceContext* c) { context_ = c; ++contexts; }
    void setOutputFormat(DXGI_FORMAT f) { format_ = f; }
    void setCommandList(ID3D12GraphicsCommandList* l) { list_ = l; ++lists; }
    void setViewport(D3D12_VIEWPORT v) { viewport_ = v; }
    void setScissorRect(D3D12_RECT r) { rect_ = r; }
protected:
    void* queryInterface(std::type_index type) override {
        if (type == typeid(Interface)) return static_cast<Interface*>(this);
        if (type == typeid(SR::IWeaverBase1)) return static_cast<SR::IWeaverBase1*>(this);
        if (type == typeid(SR::IDestroyable)) return static_cast<SR::IDestroyable*>(this);
        if (type == typeid(SR::IQueryInterface)) return static_cast<SR::IQueryInterface*>(this);
        return nullptr;
    }
private:
    ComPtr<ID3D11ShaderResourceView> input11_;
    ComPtr<ID3D12Resource> input12_;
    ComPtr<ID3D11DeviceContext> context_;
    ID3D12GraphicsCommandList* list_{};
    DXGI_FORMAT format_{};
    D3D12_VIEWPORT viewport_{}; D3D12_RECT rect_{};
    HWND window_{}; uint64_t latency_{}; bool late_{}, srgb_{};
};
// This implementation deliberately has a different complete-object address,
// virtual-base layout and field path from Fake. No fixture exports reveal any
// of its stored resources: capture must discover the real public setters.
struct Prefix {
    virtual ~Prefix() = default;
    virtual uint64_t prefixValue() const { return padding[2]; }
    std::array<uint64_t, 7> padding{1, 2, 3, 4, 5, 6, 7};
};
template<class Interface> class FakePimpl final : public Prefix, public Interface {
    struct Impl {
        std::array<uint64_t, 5> padding{};
        DXGI_FORMAT format{};
        ComPtr<ID3D12Resource> input12;
        std::array<uint64_t, 3> gap{};
        ID3D12GraphicsCommandList* list{};
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11ShaderResourceView> input11;
        D3D12_VIEWPORT viewport{}; D3D12_RECT rect{};
        HWND window{}; uint64_t latency{}; bool late{}, srgb{};
    };
    std::array<uint64_t, 11> padding_{};
    std::unique_ptr<Impl> pimpl_ = std::make_unique<Impl>();
public:
    void destroy() override { ++destroyed; delete this; }
    void setWindowHandle(HWND w) override { pimpl_->window = w; }
    void weave() override { ++calls; }
    void enableLateLatching(bool value) override { pimpl_->late = value; }
    bool isLateLatchingEnabled() const override { return pimpl_->late; }
    void setShaderSRGBConversion(bool r, bool w) override { pimpl_->srgb = r || w; }
    void setLatencyInFrames(uint64_t value) override { pimpl_->latency = value * 16666; }
    void setLatency(uint64_t value) override { pimpl_->latency = value; }
    uint64_t getLatency() const override { return pimpl_->latency; }
    void getPredictedEyePositions(float left[3], float right[3]) override { for (int i = 0; i < 3; ++i) left[i] = right[i] = 100.f + i; }
    void setInputViewTexture(ID3D11ShaderResourceView* v, int, int, DXGI_FORMAT f) { pimpl_->input11 = v; pimpl_->format = f; ++inputs; }
    void setInputViewTexture(ID3D12Resource* v, int, int, DXGI_FORMAT f) { pimpl_->input12 = v; pimpl_->format = f; ++inputs; }
    void setContext(ID3D11DeviceContext* c) { pimpl_->context = c; ++contexts; }
    void setOutputFormat(DXGI_FORMAT f) { pimpl_->format = f; }
    void setCommandList(ID3D12GraphicsCommandList* l) { pimpl_->list = l; ++lists; }
    void setViewport(D3D12_VIEWPORT v) { pimpl_->viewport = v; }
    void setScissorRect(D3D12_RECT r) { pimpl_->rect = r; }
protected:
    void* queryInterface(std::type_index type) override {
        if (type == typeid(Interface)) return static_cast<Interface*>(this);
        if (type == typeid(SR::IWeaverBase1)) return static_cast<SR::IWeaverBase1*>(this);
        if (type == typeid(SR::IDestroyable)) return static_cast<SR::IDestroyable*>(this);
        if (type == typeid(SR::IQueryInterface)) return static_cast<SR::IQueryInterface*>(this);
        return nullptr;
    }
};
}
namespace SR {
IDX11Weaver1::~IDX11Weaver1() = default;
IDX12Weaver1::~IDX12Weaver1() = default;
WeaverErrorCode CreateDX11Weaver(SRContext* context, ID3D11DeviceContext* c, HWND w, IDX11Weaver1** out) {
    if (context == reinterpret_cast<SRContext*>(1)) return WeaverTextureFailedToLoad;
    ++factories;
    IDX11Weaver1* f = layout ? static_cast<IDX11Weaver1*>(new FakePimpl<IDX11Weaver1>) : static_cast<IDX11Weaver1*>(new Fake<IDX11Weaver1>);
    f->setContext(c); f->setWindowHandle(w); *out = f; return WeaverSuccess;
}
WeaverErrorCode CreateDX12Weaver(SRContext* context, ID3D12Device*, HWND w, IDX12Weaver1** out) {
    if (context == reinterpret_cast<SRContext*>(1)) return WeaverTextureFailedToLoad;
    ++factories;
    IDX12Weaver1* f = layout ? static_cast<IDX12Weaver1*>(new FakePimpl<IDX12Weaver1>) : static_cast<IDX12Weaver1*>(new Fake<IDX12Weaver1>);
    f->setWindowHandle(w); *out = f; return WeaverSuccess;
}
class DX11WeaverBase::Impl {
public:
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    HWND window{};
};
DX11WeaverBase::DX11WeaverBase() = default;
DX11WeaverBase::~DX11WeaverBase() = default;
PredictingDX11Weaver::PredictingDX11Weaver(SRContext&, ID3D11Device* d, ID3D11DeviceContext* c, unsigned w, unsigned h, DXGI_FORMAT f, HWND hwnd) {
    pimpl = std::make_unique<Impl>(); pimpl->context = c; pimpl->window = hwnd;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = f; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&desc, nullptr, &pimpl->texture)) || FAILED(d->CreateRenderTargetView(pimpl->texture.Get(), nullptr, &pimpl->target))) throw std::runtime_error("legacy test texture");
}
PredictingDX11Weaver::~PredictingDX11Weaver() { ++destroyed; }
ID3D11RenderTargetView* DX11WeaverBase::getFrameBuffer() { return pimpl->target.Get(); }
void DX11WeaverBase::setInputFrameBuffer(ID3D11ShaderResourceView* v) {
    // The public legacy contract releases the internally managed framebuffer
    // once an external input is selected; the getter can no longer recover it.
    pimpl->target.Reset(); pimpl->texture.Reset(); pimpl->view = v; ++inputs;
}
void DX11WeaverBase::setContext(ID3D11DeviceContext* c) { pimpl->context = c; }
void DX11WeaverBase::setWindowHandle(HWND w) { pimpl->window = w; }
void DX11WeaverBase::weave(unsigned w, unsigned h) { weave(w, h, 0, 0); }
void DX11WeaverBase::weave(unsigned, unsigned, unsigned, unsigned) { ++calls; }
class DX12WeaverBase::Impl {
public:
    ComPtr<ID3D12Resource> input;
    ID3D12GraphicsCommandList* list{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    HWND window{};
};
DX12WeaverBase::DX12WeaverBase() = default;
DX12WeaverBase::~DX12WeaverBase() = default;
PredictingDX12Weaver::PredictingDX12Weaver(SRContext&, ID3D12Device*, ID3D12CommandAllocator*, ID3D12CommandQueue*, ID3D12Resource* input, ID3D12Resource*, HWND window) {
    pimpl = std::make_unique<Impl>(); pimpl->window = window; setInputFrameBuffer(input);
}
PredictingDX12Weaver::~PredictingDX12Weaver() { ++destroyed; }
ID3D12Resource* DX12WeaverBase::getFrameBuffer() { return pimpl->input.Get(); }
void DX12WeaverBase::setInputFrameBuffer(ID3D12Resource* input) { setInputFrameBuffer(input, DXGI_FORMAT_UNKNOWN); }
void DX12WeaverBase::setInputFrameBuffer(ID3D12Resource* input, DXGI_FORMAT format) { pimpl->input = input; pimpl->format = format; ++inputs; }
void DX12WeaverBase::setCommandList(ID3D12GraphicsCommandList* list) { pimpl->list = list; }
void DX12WeaverBase::setWindowHandle(HWND window) { pimpl->window = window; }
void DX12WeaverBase::weave(unsigned w, unsigned h) { weave(w, h, 0, 0); }
void DX12WeaverBase::weave(unsigned, unsigned, unsigned, unsigned) { ++calls; }
void DX12WeaverBase::weave(ID3D12GraphicsCommandList* list, unsigned w, unsigned h) { setCommandList(list); weave(w, h); }
void DX12WeaverBase::weave(ID3D12GraphicsCommandList* list, unsigned w, unsigned h, unsigned x, unsigned y) { setCommandList(list); weave(w, h, x, y); }
}
extern "C" __declspec(dllexport) unsigned FakeLeiaCalls() { return calls; }
extern "C" __declspec(dllexport) unsigned FakeLeiaDestroyed() { return destroyed; }
extern "C" __declspec(dllexport) unsigned FakeLeiaInputs() { return inputs; }
extern "C" __declspec(dllexport) unsigned FakeLeiaContexts() { return contexts; }
extern "C" __declspec(dllexport) unsigned FakeLeiaLists() { return lists; }
extern "C" __declspec(dllexport) unsigned FakeLeiaFactories() { return factories; }
extern "C" __declspec(dllexport) void FakeLeiaLayout(unsigned value) { layout = value; }
