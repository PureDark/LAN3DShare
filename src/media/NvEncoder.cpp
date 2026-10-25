#include "media/NvEncoder.h"
#include "media/CrossAdapterCopy.h"
#include "media/GpuAdapter.h"
#include "../../third_party/nvcodec/nvEncodeAPI.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <chrono>

namespace lan3d::media {
using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
double ElapsedMs(Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); }
bool HrError(std::string& error, const char* operation, HRESULT hr) {
    char text[192]{};
    sprintf_s(text, "%s failed (0x%08X)", operation, static_cast<unsigned>(hr));
    error = text;
    return false;
}
const GUID& CodecGuid(CodecId codec) {
    if (codec == CodecId::Av1) return NV_ENC_CODEC_AV1_GUID;
    if (codec == CodecId::Hevc) return NV_ENC_CODEC_HEVC_GUID;
    return NV_ENC_CODEC_H264_GUID;
}
void SetVui(NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui) {
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
}
}

struct NvEncoder::Impl {
    HMODULE module{};
    NV_ENCODE_API_FUNCTION_LIST api{};
    NV_ENC_CONFIG encodeSettings{};
    NV_ENC_INITIALIZE_PARAMS initializeParams{};
    void* session{};
    NV_ENC_REGISTERED_PTR registered{};
    NV_ENC_INPUT_PTR mapped{};
    NV_ENC_OUTPUT_PTR bitstream{};
    bool initialized{}, forceKeyframe{true}, locked{};
    uint32_t frameIndex{}, codedHeight{};
    CodecConfig config;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> bgra, nv12;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    LUID adapterLuid{}, sourceLuid{};
    std::unique_ptr<CrossAdapterCopy> crossCopy;
    GpuFrame crossDescription{};
    bool gpuInput{};
    uint32_t processorWidth{}, processorHeight{};
    DXGI_FORMAT processorFormat{DXGI_FORMAT_UNKNOWN};
    struct SharedInput {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    std::map<std::wstring, SharedInput> sharedInputs;
    ComPtr<ID3D11VideoProcessorEnumerator> previewEnumerator;
    ComPtr<ID3D11VideoProcessor> previewProcessor;
    ComPtr<ID3D11VideoProcessorInputView> previewInput;
    ComPtr<ID3D11VideoProcessorOutputView> previewOutput;
    ComPtr<ID3D11Texture2D> previewTexture, previewReadback;
    Frame latestMetadata, previewMetadata;
    ULONGLONG lastPreview{};
    bool previewPending{};
    EncodeTiming timing;
    void ReadTimer() {
        if (crossCopy) { const auto copy = crossCopy->CompletedTiming(); timing.sourceCopyGpuMs = copy.sourceMs; timing.destinationCopyGpuMs = copy.destinationMs; }
    }

    bool EncodeConverted(uint64_t sequence, uint64_t timestamp100ns, ContentKind kind,
        std::vector<EncodedFrame>& packets, std::string& error);

    ~Impl() { Reset(); }
    void Reset() {
        if (session) {
            // EOS completes any accepted input before its registered resource is
            // released, including error exits after EncodePicture.
            if (initialized && api.nvEncEncodePicture) {
                NV_ENC_PIC_PARAMS eos{};
                eos.version = NV_ENC_PIC_PARAMS_VER;
                eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
                api.nvEncEncodePicture(session, &eos);
            }
            if (locked && api.nvEncUnlockBitstream) api.nvEncUnlockBitstream(session, bitstream);
            if (mapped && api.nvEncUnmapInputResource) api.nvEncUnmapInputResource(session, mapped);
            if (registered && api.nvEncUnregisterResource) api.nvEncUnregisterResource(session, registered);
            if (bitstream && api.nvEncDestroyBitstreamBuffer) api.nvEncDestroyBitstreamBuffer(session, bitstream);
            if (api.nvEncDestroyEncoder) api.nvEncDestroyEncoder(session);
        }
        session = nullptr; registered = nullptr; mapped = nullptr; bitstream = nullptr;
        initialized = false; locked = false; forceKeyframe = true; frameIndex = 0; codedHeight = 0;
        sharedInputs.clear();
        previewInput.Reset(); previewOutput.Reset(); previewProcessor.Reset(); previewEnumerator.Reset();
        previewTexture.Reset(); previewReadback.Reset();
        latestMetadata = {}; previewMetadata = {}; lastPreview = 0; previewPending = false;
        timing = {};
        outputView.Reset(); inputView.Reset(); nv12.Reset(); bgra.Reset();
        processor.Reset(); enumerator.Reset(); videoContext.Reset(); videoDevice.Reset();
        if (context) { context->ClearState(); context->Flush(); }
        context.Reset(); device.Reset();
        crossCopy.reset(); sourceLuid = {}; adapterLuid = {}; gpuInput = false;
        processorWidth = processorHeight = 0; processorFormat = DXGI_FORMAT_UNKNOWN;
        api = {};
        encodeSettings = {}; initializeParams = {};
        if (module) FreeLibrary(module);
        module = nullptr;
        config = {};
    }
    bool NvError(std::string& error, const char* operation, NVENCSTATUS status) {
        char text[192]{};
        sprintf_s(text, "%s failed (NVENC %u)", operation, static_cast<unsigned>(status));
        error = text;
        if (session && api.nvEncGetLastErrorString) {
            const char* detail = api.nvEncGetLastErrorString(session);
            if (detail && *detail) { error += ": "; error += detail; }
        }
        return false;
    }
    bool Load(std::string& error) {
        module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { error = "NVIDIA NVENC driver is unavailable (nvEncodeAPI64.dll)"; return false; }
        using GetVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
        using CreateApi = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        auto version = reinterpret_cast<GetVersion>(GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create = reinterpret_cast<CreateApi>(GetProcAddress(module, "NvEncodeAPICreateInstance"));
        uint32_t supported{};
        if (!version || !create) { error = "NVIDIA NVENC driver exports are incomplete"; return false; }
        NVENCSTATUS status = version(&supported);
        if (status != NV_ENC_SUCCESS) return NvError(error, "NVENC API version query", status);
        if (supported < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
            error = "NVIDIA driver does not support NVENC API 13.0; update the display driver";
            return false;
        }
        api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        status = create(&api);
        if (status != NV_ENC_SUCCESS) return NvError(error, "NVENC API creation", status);
        if (!api.nvEncOpenEncodeSessionEx || !api.nvEncGetEncodeCaps || !api.nvEncGetEncodePresetConfigEx ||
            !api.nvEncInitializeEncoder || !api.nvEncRegisterResource || !api.nvEncMapInputResource ||
            !api.nvEncUnmapInputResource || !api.nvEncUnregisterResource || !api.nvEncCreateBitstreamBuffer ||
            !api.nvEncDestroyBitstreamBuffer || !api.nvEncGetSequenceParams || !api.nvEncEncodePicture ||
            !api.nvEncLockBitstream || !api.nvEncUnlockBitstream || !api.nvEncDestroyEncoder) {
            error = "NVIDIA NVENC function table is incomplete";
            return false;
        }
        return true;
    }
    bool OpenSession(std::string& error) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = device.Get();
        const auto status = api.nvEncOpenEncodeSessionEx(&open, &session);
        return status == NV_ENC_SUCCESS || NvError(error, "open NVIDIA D3D11 encode session", status);
    }
    bool OpenDevice(std::string& error, const LUID* requestedAdapter = nullptr) {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return HrError(error, "DXGI factory", hr);
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) continue;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10DE || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (requestedAdapter && (desc.AdapterLuid.LowPart != requestedAdapter->LowPart ||
                desc.AdapterLuid.HighPart != requestedAdapter->HighPart)) continue;
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
            if (FAILED(hr)) continue;
            if (OpenSession(error)) {
                adapterLuid = desc.AdapterLuid;
                return true;
            }
            context.Reset(); device.Reset();
        }
        if (error.empty()) error = requestedAdapter ? "The capture adapter has no usable NVIDIA encoder" : "No usable NVIDIA D3D11 hardware encoder was found";
        return false;
    }
    bool EnsureProcessor(uint32_t inputWidth, uint32_t inputHeight, DXGI_FORMAT inputFormat, std::string& error) {
        if (processor && processorWidth == inputWidth && processorHeight == inputHeight && processorFormat == inputFormat) return true;
        inputView.Reset(); outputView.Reset(); processor.Reset(); enumerator.Reset();
        for (auto& [name, input] : sharedInputs) input.view.Reset();
        HRESULT hr{};
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = { config.fps, 1 };
        desc.OutputFrameRate = desc.InputFrameRate;
        desc.InputWidth = inputWidth; desc.InputHeight = inputHeight;
        desc.OutputWidth = config.width; desc.OutputHeight = codedHeight;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(hr = videoDevice->CreateVideoProcessorEnumerator(&desc, &enumerator)))
            return HrError(error, "create RGB/NV12 video enumerator", hr);
        UINT inputSupport{}, outputSupport{};
        enumerator->CheckVideoProcessorFormat(inputFormat, &inputSupport);
        enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputSupport);
        if (!(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            error = "NVIDIA D3D11 video processor does not support the source format to NV12 conversion";
            return false;
        }
        if (FAILED(hr = videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor)))
            return HrError(error, "create RGB/NV12 video processor", hr);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
        output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(hr = videoDevice->CreateVideoProcessorOutputView(nv12.Get(), enumerator.Get(), &output, &outputView)))
            return HrError(error, "create NV12 video output view", hr);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        RECT sourceRect{ 0, 0, static_cast<LONG>(inputWidth), static_cast<LONG>(inputHeight) };
        RECT outputRect{ 0, 0, static_cast<LONG>(config.width), static_cast<LONG>(codedHeight) };
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &outputRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &outputRect);
        ComPtr<ID3D11VideoContext1> video1;
        if (SUCCEEDED(videoContext.As(&video1))) {
            video1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            video1->VideoProcessorSetOutputColorSpace1(processor.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{}, yuv{};
            rgb.Nominal_Range = 2; // full 0..255
            yuv.YCbCr_Matrix = 1; yuv.Nominal_Range = 1; // BT.709 studio 16..235
            videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &rgb);
            videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &yuv);
        }
        processorWidth = inputWidth; processorHeight = inputHeight; processorFormat = inputFormat;
        return true;
    }
    bool ConvertResources(std::string& error) {
        HRESULT hr = device.As(&videoDevice);
        if (FAILED(hr)) return HrError(error, "D3D11 video device", hr);
        if (FAILED(hr = context.As(&videoContext))) return HrError(error, "D3D11 video context", hr);
        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = config.width; tex.Height = codedHeight; tex.MipLevels = 1; tex.ArraySize = 1;
        tex.SampleDesc.Count = 1; tex.Usage = D3D11_USAGE_DEFAULT;
        tex.Format = DXGI_FORMAT_NV12; tex.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(hr = device->CreateTexture2D(&tex, nullptr, &nv12))) return HrError(error, "create NV12 encode texture", hr);
        if (gpuInput) return true; // No upload texture or CPU image allocation.
        tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(hr = device->CreateTexture2D(&tex, nullptr, &bgra))) return HrError(error, "create BGRA upload texture", hr);
        if (!EnsureProcessor(config.width, codedHeight, tex.Format, error)) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
        input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(hr = videoDevice->CreateVideoProcessorInputView(bgra.Get(), enumerator.Get(), &input, &inputView)))
            return HrError(error, "create BGRA video input view", hr);
        return true;
    }
    bool PreviewResources() {
        const double scale = 432. / std::max(config.width, config.height);
        const UINT width = std::max(4u, UINT(config.width * scale) & ~1u);
        const UINT height = std::max(2u, UINT(config.height * scale) & ~1u);
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = content.OutputFrameRate = {1, 1};
        content.InputWidth = config.width; content.InputHeight = codedHeight;
        content.OutputWidth = width; content.OutputHeight = height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&content, &previewEnumerator)) ||
            FAILED(videoDevice->CreateVideoProcessor(previewEnumerator.Get(), 0, &previewProcessor))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &previewTexture))) return false;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &previewReadback))) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{}; in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{}; out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(nv12.Get(), previewEnumerator.Get(), &in, &previewInput)) ||
            FAILED(videoDevice->CreateVideoProcessorOutputView(previewTexture.Get(), previewEnumerator.Get(), &out, &previewOutput))) return false;
        videoContext->VideoProcessorSetStreamAutoProcessingMode(previewProcessor.Get(), 0, FALSE);
        videoContext->VideoProcessorSetStreamFrameFormat(previewProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        const RECT source{0, 0, LONG(config.width), LONG(config.height)}, target{0, 0, LONG(width), LONG(height)};
        videoContext->VideoProcessorSetStreamSourceRect(previewProcessor.Get(), 0, TRUE, &source);
        videoContext->VideoProcessorSetStreamDestRect(previewProcessor.Get(), 0, TRUE, &target);
        videoContext->VideoProcessorSetOutputTargetRect(previewProcessor.Get(), TRUE, &target);
        ComPtr<ID3D11VideoContext1> video1;
        if (FAILED(videoContext.As(&video1))) return false;
        video1->VideoProcessorSetStreamColorSpace1(previewProcessor.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        video1->VideoProcessorSetOutputColorSpace1(previewProcessor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        previewMetadata.width = width; previewMetadata.height = height; previewMetadata.stride = width * 4;
        return true;
    }
    bool Initialize(std::string& error) {
        const GUID& guid = CodecGuid(config.codec);
        NV_ENC_CAPS_PARAM caps{};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        int maxWidth{}, maxHeight{};
        caps.capsToQuery = NV_ENC_CAPS_WIDTH_MAX;
        NVENCSTATUS status = api.nvEncGetEncodeCaps(session, guid, &caps, &maxWidth);
        if (status != NV_ENC_SUCCESS) return NvError(error, "query NVIDIA codec width", status);
        caps.capsToQuery = NV_ENC_CAPS_HEIGHT_MAX;
        status = api.nvEncGetEncodeCaps(session, guid, &caps, &maxHeight);
        if (status != NV_ENC_SUCCESS) return NvError(error, "query NVIDIA codec height", status);
        if (maxWidth < static_cast<int>(config.width) || maxHeight < static_cast<int>(codedHeight)) {
            error = "Selected NVIDIA codec cannot encode the requested dimensions";
            return false;
        }
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        status = api.nvEncGetEncodePresetConfigEx(session, guid, NV_ENC_PRESET_P5_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY, &preset);
        if (status != NV_ENC_SUCCESS) return NvError(error, "get NVENC P5 low latency preset", status);
        NV_ENC_CONFIG settings = preset.presetCfg;
        settings.version = NV_ENC_CONFIG_VER;
        settings.gopLength = config.fps * 2;
        settings.frameIntervalP = 1;
        settings.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
        auto& rc = settings.rcParams;
        rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        rc.averageBitRate = rc.maxBitRate = config.bitrate;
        rc.vbvBufferSize = config.bitrate / config.fps * 2;
        rc.vbvInitialDelay = rc.vbvBufferSize;
        rc.enableLookahead = 0; rc.lookaheadDepth = 0; rc.enableExtLookahead = 0;
        rc.enableTemporalAQ = 0; rc.enableAQ = 1; rc.aqStrength = 8;
        rc.zeroReorderDelay = 1;
        rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        if (config.codec == CodecId::Av1) {
            settings.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
            auto& av1 = settings.encodeCodecConfig.av1Config;
            av1.idrPeriod = settings.gopLength;
            av1.chromaFormatIDC = 1;
            av1.inputBitDepth = av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
            av1.repeatSeqHdr = 1; av1.disableSeqHdr = 0; av1.outputAnnexBFormat = 0;
            av1.enableBitstreamPadding = 0;
            av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
            av1.colorRange = 0;
        } else if (config.codec == CodecId::Hevc) {
            settings.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
            auto& hevc = settings.encodeCodecConfig.hevcConfig;
            hevc.idrPeriod = settings.gopLength;
            hevc.chromaFormatIDC = 1;
            hevc.inputBitDepth = hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
            hevc.repeatSPSPPS = 1; hevc.disableSPSPPS = 0;
            SetVui(hevc.hevcVUIParameters);
        } else {
            settings.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            auto& h264 = settings.encodeCodecConfig.h264Config;
            h264.idrPeriod = settings.gopLength;
            h264.chromaFormatIDC = 1;
            h264.inputBitDepth = h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
            h264.repeatSPSPPS = 1; h264.disableSPSPPS = 0;
            SetVui(h264.h264VUIParameters);
        }
        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = guid;
        init.presetGUID = NV_ENC_PRESET_P5_GUID;
        init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        init.encodeWidth = init.darWidth = init.maxEncodeWidth = config.width;
        init.encodeHeight = init.darHeight = init.maxEncodeHeight = codedHeight;
        init.frameRateNum = config.fps; init.frameRateDen = 1;
        init.enablePTD = 1; init.enableEncodeAsync = 0;
        init.encodeConfig = &settings;
        // Keep P5/AQ/CBR while using two NVENC engines for shared GPU 4K input.
        // CPU input, smaller pictures and unsupported devices retain AUTO.
        // Explicit overrides are diagnostic; 0 preserves AUTO and 2 requires split.
        wchar_t splitOption[16]{};
        const DWORD splitLength = gpuInput ? GetEnvironmentVariableW(L"LAN3D_NVENC_SPLIT", splitOption, 16) : 0;
        int splitOverride = -1, encoderEngines = -1;
        if (splitLength) {
            if (splitLength != 1 || (splitOption[0] != L'0' && splitOption[0] != L'2')) {
                error = "Diagnostic LAN3D_NVENC_SPLIT only accepts 0 (AUTO) or 2 (two strips)";
                return false;
            }
            splitOverride = splitOption[0] - L'0';
        }
        const bool splitCodec = config.codec == CodecId::Hevc || config.codec == CodecId::Av1;
        const bool splitCompatible = splitCodec && !init.enableOutputInVidmem &&
            (config.codec != CodecId::Hevc || (!init.enableWeightedPrediction && !init.enableSubFrameWrite &&
                !settings.encodeCodecConfig.hevcConfig.enableAlphaLayerEncoding));
        NVENCSTATUS engineStatus = NV_ENC_ERR_UNSUPPORTED_PARAM;
        if (gpuInput && splitCodec) {
            caps.capsToQuery = NV_ENC_CAPS_NUM_ENCODER_ENGINES;
            engineStatus = api.nvEncGetEncodeCaps(session, guid, &caps, &encoderEngines);
            if (engineStatus != NV_ENC_SUCCESS) encoderEngines = -1;
        }
        if (splitOverride == 2) {
            if (!splitCodec) { error = "Diagnostic NVENC split encoding requires HEVC or AV1"; return false; }
            if (engineStatus != NV_ENC_SUCCESS) return NvError(error, "query NVIDIA encoding engine count", engineStatus);
            if (encoderEngines < 2) {
                error = "Diagnostic NVENC two-strip encoding requires at least two encoding engines";
                return false;
            }
            if (!splitCompatible) {
                error = "Diagnostic NVENC split encoding conflicts with the configured encoding features";
                return false;
            }
        }
        const bool automaticSplit = gpuInput && splitOverride < 0 && config.width >= 3840 && config.height >= 2160 &&
            splitCompatible && engineStatus == NV_ENC_SUCCESS && encoderEngines >= 2;
        if (automaticSplit || splitOverride == 2)
            init.splitEncodeMode = NV_ENC_SPLIT_TWO_FORCED_MODE;
        // Keep untouched input parameters for the fresh-session fallback.
        const auto requestedSettings = settings;
        const auto requestedInit = init;
        bool splitFallback{};
        status = api.nvEncInitializeEncoder(session, &init);
        if (status != NV_ENC_SUCCESS && automaticSplit) {
            const auto splitStatus = status;
            // No resources have been registered and no frame has been submitted.
            // Never retry InitializeEncoder on a possibly half-initialized session.
            status = api.nvEncDestroyEncoder(session);
            if (status != NV_ENC_SUCCESS) return NvError(error, "destroy failed NVIDIA split session", status);
            session = nullptr;
            if (!OpenSession(error)) return false;
            settings = requestedSettings; init = requestedInit;
            init.encodeConfig = &settings; init.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;
            splitFallback = true;
            std::printf("NVENC_MODE split_fallback=1 failed_status=%u session_recreated=1 codec=%s\n",
                static_cast<unsigned>(splitStatus), CodecName(config.codec));
            std::fflush(stdout);
            status = api.nvEncInitializeEncoder(session, &init);
        }
        if (status != NV_ENC_SUCCESS) return NvError(error, "initialize NVIDIA hardware encoder", status);
        if (gpuInput) {
            std::printf("NVENC_MODE split_mode=%u selection=%s encoder_engines=%d caps_status=%u init_accepted=1 codec=%s width=%u height=%u preset=P5 spatial_aq=1 tuning=low_latency\n",
                static_cast<unsigned>(init.splitEncodeMode), splitFallback ? "fallback" : splitLength ? "override" : "automatic",
                encoderEngines, static_cast<unsigned>(engineStatus), CodecName(config.codec), config.width, config.height);
            std::fflush(stdout);
        }
        encodeSettings = settings;
        initializeParams = init;
        initializeParams.encodeConfig = &encodeSettings;
        initialized = true;
        if (!ConvertResources(error)) return false;
        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.width = config.width; resource.height = codedHeight;
        resource.resourceToRegister = nv12.Get();
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        status = api.nvEncRegisterResource(session, &resource);
        if (status != NV_ENC_SUCCESS) return NvError(error, "register NV12 NVIDIA encode resource", status);
        registered = resource.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
        buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = api.nvEncCreateBitstreamBuffer(session, &buffer);
        if (status != NV_ENC_SUCCESS) return NvError(error, "create NVIDIA bitstream buffer", status);
        bitstream = buffer.bitstreamBuffer;
        return ReadHeader(error);
    }
    bool ReadHeader(std::string& error) {
        std::vector<uint8_t> header(MaxSequenceHeaderBytes);
        uint32_t bytes{};
        NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
        payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
        payload.inBufferSize = static_cast<uint32_t>(header.size());
        payload.spsppsBuffer = header.data();
        payload.outSPSPPSPayloadSize = &bytes;
        const auto status = api.nvEncGetSequenceParams(session, &payload);
        if (status != NV_ENC_SUCCESS) return NvError(error, "get NVIDIA stream sequence header", status);
        if (!bytes || bytes > MaxSequenceHeaderBytes) { error = "NVIDIA returned an invalid sequence header"; return false; }
        header.resize(bytes);
        config.sequenceHeader = std::move(header);
        return true;
    }
};

NvEncoder::NvEncoder() : impl_(std::make_unique<Impl>()) {}
NvEncoder::~NvEncoder() = default;

uint32_t NvEncoder::Probe() {
    uint32_t mask{};
    for (CodecId codec : { CodecId::Av1, CodecId::Hevc, CodecId::H264 }) {
        NvEncoder probe;
        CodecConfig config;
        config.width = 3840; config.height = 2160; config.fps = 60;
        config.bitrate = 100000000; config.codec = codec;
        std::string error;
        if (probe.Configure(config, error)) mask |= CodecBit(codec);
    }
    return mask;
}
bool NvEncoder::Configure(const CodecConfig& config, std::string& error) {
    error.clear();
    auto& p = *impl_;
    p.Reset();
    if (!ValidConfig(config) || !CodecBit(config.codec)) {
        error = "Invalid NVIDIA video encode configuration";
        return false;
    }
    p.config = config;
    p.config.sequenceHeader.clear();
    p.codedHeight = (config.height + 1u) & ~1u;
    if (!p.Load(error) || !p.OpenDevice(error) || !p.Initialize(error)) { p.Reset(); return false; }
    return true;
}
bool NvEncoder::ConfigureGpu(const CodecConfig& config, const GpuFrame& input, std::string& error, const LUID* adapter) {
    error.clear();
    auto& p = *impl_;
    p.Reset();
    if (!ValidConfig(config) || !ValidGpuFrame(input)) {
        error = "Invalid NVIDIA shared GPU input configuration";
        return false;
    }
    p.config = config;
    p.config.sequenceHeader.clear();
    p.codedHeight = (config.height + 1u) & ~1u;
    p.gpuInput = true; p.sourceLuid = input.adapterLuid;
    if (!p.Load(error) || !p.OpenDevice(error, adapter ? adapter : &input.adapterLuid) || !p.Initialize(error) ||
        !p.EnsureProcessor(input.width, input.height, input.format, error)) {
        p.Reset(); return false;
    }
    if (!SameAdapter(p.sourceLuid, p.adapterLuid)) {
        p.crossCopy = std::make_unique<CrossAdapterCopy>();
        if (!p.crossCopy->Open(input, p.device.Get(), error)) { p.Reset(); return false; }
        p.crossDescription = input;
    }
    printf("GPU_ENCODER source=%08X:%08X encoder=%08X:%08X cross_adapter=%u copy_queues=%u\n",
        unsigned(p.sourceLuid.HighPart), p.sourceLuid.LowPart, unsigned(p.adapterLuid.HighPart), p.adapterLuid.LowPart,
        p.crossCopy ? 1u : 0u, p.crossCopy ? 2u : 0u); fflush(stdout);
    return true;
}
bool NvEncoder::UpdateBitrate(uint32_t bitrate, std::string& error) {
    error.clear();
    auto& p = *impl_;
    if (!p.initialized || !p.api.nvEncReconfigureEncoder || bitrate < 64000 || bitrate > 1000000000) {
        error = "NVENC dynamic bitrate is unavailable or the target is out of range";
        return false;
    }
    if (bitrate == p.config.bitrate) return true;
    auto settings = p.encodeSettings;
    settings.rcParams.averageBitRate = settings.rcParams.maxBitRate = bitrate;
    settings.rcParams.vbvBufferSize = bitrate / p.config.fps * 2;
    settings.rcParams.vbvInitialDelay = settings.rcParams.vbvBufferSize;
    NV_ENC_RECONFIGURE_PARAMS reconfigure{};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfigure.reInitEncodeParams = p.initializeParams;
    reconfigure.reInitEncodeParams.encodeConfig = &settings;
    reconfigure.forceIDR = 1;
    const auto status = p.api.nvEncReconfigureEncoder(p.session, &reconfigure);
    if (status != NV_ENC_SUCCESS) return p.NvError(error, "reconfigure NVENC bitrate", status);
    p.encodeSettings = settings;
    p.config.bitrate = bitrate;
    p.forceKeyframe = true;
    return p.ReadHeader(error);
}
bool NvEncoder::Encode(const Frame& frame, std::vector<EncodedFrame>& packets, std::string& error) {
    error.clear(); packets.clear();
    auto& p = *impl_;
    const auto started = Clock::now(); p.timing = {};
    if (!p.initialized || p.gpuInput) { error = "NVIDIA CPU-input encoder is not configured"; return false; }
    if (!ValidFrame(frame) || frame.width != p.config.width || frame.height != p.config.height) {
        error = "Frame dimensions or data do not match the NVIDIA encoder";
        return false;
    }
    D3D11_BOX imageBox{ 0, 0, 0, frame.width, frame.height, 1 };
    p.context->UpdateSubresource(p.bgra.Get(), 0, &imageBox, frame.bgra.data(), frame.stride, 0);
    if (p.codedHeight != frame.height) {
        // NV12 needs paired rows. Repeat the final visible row on the GPU;
        // the wire configuration retains the original visible height.
        D3D11_BOX lastRow{ 0, frame.height, 0, frame.width, p.codedHeight, 1 };
        p.context->UpdateSubresource(p.bgra.Get(), 0, &lastRow,
            frame.bgra.data() + size_t(frame.stride) * (frame.height - 1), frame.stride, 0);
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE; stream.pInputSurface = p.inputView.Get();
    HRESULT hr = p.videoContext->VideoProcessorBlt(p.processor.Get(), p.outputView.Get(), p.frameIndex, 1, &stream);
    if (FAILED(hr)) return HrError(error, "GPU RGB to NV12 conversion", hr);
    p.timing.inputSubmitMs = ElapsedMs(started);
    const bool encoded = p.EncodeConverted(frame.sequence, frame.timestamp100ns, frame.kind, packets, error);
    if (encoded) for (auto& packet : packets) {
        packet.displayWidth = frame.displayWidth; packet.displayHeight = frame.displayHeight;
    }
    if (encoded) p.ReadTimer();
    p.timing.totalMs = ElapsedMs(started);
    return encoded;
}
bool NvEncoder::EncodeGpu(const GpuFrame& frame, std::vector<EncodedFrame>& packets, std::string& error) {
    error.clear(); packets.clear();
    auto& p = *impl_;
    const auto started = Clock::now(); p.timing = {};
    if (!p.initialized || !p.gpuInput) { error = "NVIDIA shared GPU-input encoder is not configured"; return false; }
    if (!ValidGpuFrame(frame) || frame.adapterLuid.LowPart != p.sourceLuid.LowPart ||
        frame.adapterLuid.HighPart != p.sourceLuid.HighPart) {
        error = "Shared GPU frame is invalid or belongs to a different adapter";
        return false;
    }
    if (!p.EnsureProcessor(frame.width, frame.height, frame.format, error)) return false;
    ID3D11Texture2D* transferred{};
    if (p.crossCopy) {
        if (frame.width != p.crossDescription.width || frame.height != p.crossDescription.height || frame.format != p.crossDescription.format) {
            p.sharedInputs.clear();
            if (!p.crossCopy->Open(frame, p.device.Get(), error)) return false;
            p.crossDescription = frame;
        }
        if (!p.crossCopy->Copy(frame, &transferred, error)) return false;
    }
    const std::wstring name = p.crossCopy ? L"cross-adapter" : frame.textureName;
    auto found = p.sharedInputs.find(name);
    if (found == p.sharedInputs.end()) {
        // Previous successful calls finished every read before returning. A
        // failed encode tears the session down or causes the sender to stop.
        if (p.sharedInputs.size() >= 24) p.sharedInputs.clear();
        ComPtr<ID3D11Device1> device1;
        HRESULT hr = p.device.As(&device1);
        if (FAILED(hr)) return HrError(error, "D3D11 shared texture device", hr);
        Impl::SharedInput input;
        if (transferred) input.texture = transferred;
        else if (FAILED(hr = device1->OpenSharedResourceByName(name.c_str(), DXGI_SHARED_RESOURCE_READ,
            IID_PPV_ARGS(&input.texture)))) return HrError(error, "open shared GPU capture texture", hr);
        found = p.sharedInputs.emplace(name, std::move(input)).first;
    }
    auto& input = found->second;
    D3D11_TEXTURE2D_DESC desc{};
    input.texture->GetDesc(&desc);
    if (desc.Width != frame.width || desc.Height != frame.height || desc.Format != frame.format ||
        desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
        error = "Shared GPU capture texture does not match its metadata";
        return false;
    }
    HRESULT hr{};
    if (!input.view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(hr = p.videoDevice->CreateVideoProcessorInputView(input.texture.Get(), p.enumerator.Get(), &view, &input.view)))
            return HrError(error, "create shared GPU video input view", hr);
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE; stream.pInputSurface = input.view.Get();
    hr = p.videoContext->VideoProcessorBlt(p.processor.Get(), p.outputView.Get(), p.frameIndex, 1, &stream);
    if (FAILED(hr)) return HrError(error, "GPU capture scaling and NV12 conversion", hr);
    // NVENC consumes the converted texture on this same D3D11 device. Its
    // synchronous bitstream completion also completes the preceding VP read
    // of the source, so only a successful return permits releasing the lease.
    p.timing.inputSubmitMs = ElapsedMs(started);
    const bool encoded = p.EncodeConverted(frame.sequence, frame.timestamp100ns, frame.kind, packets, error);
    if (encoded) for (auto& packet : packets) {
        packet.displayWidth = frame.kind == ContentKind::LeiaSbs ? frame.width / 2 : frame.width;
        packet.displayHeight = frame.height;
    }
    if (encoded) p.ReadTimer();
    p.timing.totalMs = ElapsedMs(started);
    return encoded;
}
bool NvEncoder::Impl::EncodeConverted(uint64_t sequence, uint64_t timestamp100ns, ContentKind kind,
    std::vector<EncodedFrame>& packets, std::string& error) {
    const auto submitStarted = Clock::now();
    context->Flush();
    timing.flushMs = ElapsedMs(submitStarted);
    NV_ENC_MAP_INPUT_RESOURCE mapping{};
    mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapping.registeredResource = registered;
    const auto mapStarted = Clock::now();
    NVENCSTATUS status = api.nvEncMapInputResource(session, &mapping);
    timing.mapInputMs = ElapsedMs(mapStarted);
    if (status != NV_ENC_SUCCESS) return NvError(error, "map NVIDIA encode input", status);
    mapped = mapping.mappedResource;
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = config.width; picture.inputHeight = codedHeight;
    picture.inputBuffer = mapped; picture.bufferFmt = mapping.mappedBufferFmt;
    picture.outputBitstream = bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.frameIdx = frameIndex++;
    picture.inputTimeStamp = timestamp100ns;
    picture.inputDuration = 10000000ull / config.fps;
    if (forceKeyframe) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    const auto encodeStarted = Clock::now();
    status = api.nvEncEncodePicture(session, &picture);
    timing.encodePictureMs = ElapsedMs(encodeStarted);
    timing.encodeSubmitMs = ElapsedMs(submitStarted);
    if (status != NV_ENC_SUCCESS) {
        NvError(error, "encode NVIDIA low latency frame", status);
        Reset(); // Never reuse a surface accepted but held by the encoder.
        return false;
    }
    NV_ENC_LOCK_BITSTREAM output{};
    output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = bitstream;
    output.doNotWait = 0;
    const auto waitStarted = Clock::now();
    status = api.nvEncLockBitstream(session, &output);
    timing.bitstreamWaitMs = ElapsedMs(waitStarted);
    if (status != NV_ENC_SUCCESS) { NvError(error, "read NVIDIA bitstream", status); Reset(); return false; }
    locked = true;
    if (!output.bitstreamBufferPtr || !output.bitstreamSizeInBytes || output.bitstreamSizeInBytes > MaxEncodedBytes ||
        output.outputTimeStamp != timestamp100ns) {
        error = "NVIDIA bitstream size or timestamp is invalid for the zero reorder stream";
        Reset(); return false;
    }
    EncodedFrame packet;
    packet.config = config;
    packet.sequence = sequence; packet.timestamp100ns = timestamp100ns; packet.kind = kind;
    packet.keyframe = output.pictureType == NV_ENC_PIC_TYPE_IDR;
    const auto* bytes = static_cast<const uint8_t*>(output.bitstreamBufferPtr);
    packet.data.assign(bytes, bytes + output.bitstreamSizeInBytes);
    status = api.nvEncUnlockBitstream(session, bitstream);
    if (status != NV_ENC_SUCCESS) { NvError(error, "unlock NVIDIA bitstream", status); Reset(); return false; }
    locked = false;
    status = api.nvEncUnmapInputResource(session, mapped);
    if (status != NV_ENC_SUCCESS) { NvError(error, "unmap NVIDIA encode input", status); Reset(); return false; }
    mapped = nullptr;
    forceKeyframe = false;
    latestMetadata.sequence = sequence; latestMetadata.timestamp100ns = timestamp100ns; latestMetadata.kind = kind;
    packets.push_back(std::move(packet));
    return true;
}
bool NvEncoder::GpuPreview(Frame& thumbnail) {
    auto& p = *impl_;
    if (!p.initialized || !p.gpuInput || !p.latestMetadata.sequence) return false;
    try {
        if (p.previewPending) {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(p.context->Map(p.previewReadback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) return false;
            Frame frame = p.previewMetadata;
            try {
                frame.bgra.resize(size_t(frame.stride) * frame.height);
                for (uint32_t y = 0; y < frame.height; ++y)
                    memcpy(frame.bgra.data() + size_t(y) * frame.stride,
                        static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, frame.stride);
            } catch (...) { p.context->Unmap(p.previewReadback.Get(), 0); throw; }
            p.context->Unmap(p.previewReadback.Get(), 0);
            p.previewPending = false; thumbnail = std::move(frame);
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (p.lastPreview && now - p.lastPreview < 1000) return false;
        p.lastPreview = now;
        if (!p.previewOutput && !p.PreviewResources()) {
            p.previewOutput.Reset();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = p.previewInput.Get();
        if (FAILED(p.videoContext->VideoProcessorBlt(p.previewProcessor.Get(), p.previewOutput.Get(), 0, 1, &stream))) return false;
        p.context->CopyResource(p.previewReadback.Get(), p.previewTexture.Get());
        p.context->Flush();
        p.previewMetadata.sequence = p.latestMetadata.sequence;
        p.previewMetadata.timestamp100ns = p.latestMetadata.timestamp100ns;
        p.previewMetadata.kind = p.latestMetadata.kind;
        p.previewPending = true;
    } catch (...) { /* Thumbnail work must never interrupt video sharing. */ }
    return false;
}
bool NvEncoder::RequestKeyFrame(std::string& error) {
    error.clear();
    if (!impl_->initialized) { error = "NVIDIA encoder is not configured"; return false; }
    impl_->forceKeyframe = true;
    return true;
}
const CodecConfig& NvEncoder::config() const { return impl_->config; }
EncodeTiming NvEncoder::lastTiming() const { return impl_->timing; }
}
