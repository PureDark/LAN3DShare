#pragma once
#include "LeiaRuntime.h"
#include "common/Frame.h"
#include "common/GpuImage.h"
#include <map>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace lan3d::receiver {
class Renderer {
public:
    ~Renderer();
    void Open(HWND window, HMONITOR monitor, bool preview, const std::filesystem::path& runtime, bool autoDisplay = false);
    void Upload(const Frame& frame);
    LUID AdapterLuid() const;
    bool Thumbnail(const Frame& metadata, Frame& thumbnail);
    bool SetFullscreen(bool fullscreen, std::string& error);
    void SetStereoEnabled(bool enabled);
    bool Draw(bool verifyPreview = false);
    // Draw's result also serves hidden renderer tests; audio needs the stricter
    // result of a visible, non-occluded Present with a ready video image.
    bool LastPresentSucceeded() const { return lastPresentSucceeded_; }
    bool HasFrame() const { return input_ != nullptr; }
    ContentKind Content() const { return kind_; }
    bool IsWeaving() const { return runtimeActive_; }
    const char* Mode() const { return runtimeActive_ ? runtime_.Mode() : preview_ && stereoEnabled_ && kind_ == ContentKind::LeiaSbs ? "SBS PREVIEW (2D)" : IsLeia(kind_) ? "LEFT EYE (2D)" : "DESKTOP (2D)"; }
private:
    void Resize();
    void DrainGpu() noexcept;
    void VerifyPreview();
    void DisableRuntime() noexcept;
    HWND window_{};
    bool preview_{}, autoDisplay_{}, runtimeActive_{}, lastPresentSucceeded_{};
    bool stereoEnabled_{true};
    ContentKind kind_{ContentKind::Desktop};
    std::filesystem::path runtimeDirectory_;
    unsigned width_{}, height_{}, inputWidth_{}, inputHeight_{};
    unsigned displayWidth_{}, displayHeight_{};
    uint8_t verifyLeft_[4]{}, verifyRight_[4]{};
    struct GpuRead { std::shared_ptr<GpuImage> lease; Microsoft::WRL::ComPtr<ID3D11Query> complete; };
    std::vector<GpuRead> gpuReads_;
    std::map<std::wstring, Microsoft::WRL::ComPtr<ID3D11Texture2D>> sharedInputs_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> thumbnailTarget_, thumbnailReadback_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> thumbnailView_;
    Frame thumbnailMetadata_;
    uint64_t lastThumbnail_{};
    bool thumbnailPending_{};
    DXGI_FORMAT inputFormat_{DXGI_FORMAT_UNKNOWN};
    LeiaRuntime runtime_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> dc_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputView_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelLeft_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
};
}
