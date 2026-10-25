#pragma once
#include "common/GpuImage.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace lan3d::media {
// NVDEC copies its NV12 surface into Input() through CUDA/D3D11 interop.
// D3D11 compute converts that GPU buffer to a shared BGRA texture.
class DecodedGpuOutput {
public:
    bool Open(LUID adapter, uint32_t width, uint32_t height, std::string& error);
    ID3D11Buffer* Input() const { return input_.Get(); }
    bool Convert(Frame& frame, std::string& error);
private:
    struct Slot;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> input_, constants_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputView_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Query> complete_;
    std::vector<std::shared_ptr<Slot>> slots_;
    LUID adapter_{};
    uint32_t width_{}, height_{};
};
}
