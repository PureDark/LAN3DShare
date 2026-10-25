#include "media/Codec.h"
#include "media/NvEncoder.h"
#include "media/NvDecoder.h"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

using Microsoft::WRL::ComPtr;
namespace lan3d::media {
namespace {
bool Fail(std::string& error, const char* operation, HRESULT hr) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", operation, static_cast<unsigned long>(hr));
    error = text;
    return false;
}
struct Runtime {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : com;
    ~Runtime() { if (SUCCEEDED(mf)) MFShutdown(); if (SUCCEEDED(com)) CoUninitialize(); }
};
bool SetCodec(ICodecAPI* api, const GUID& property, VARTYPE type, ULONG value, std::string& error) {
    VARIANT v{};
    v.vt = type;
    if (type == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else v.ulVal = value;
    const HRESULT hr = api->SetValue(&property, &v);
    return SUCCEEDED(hr) || Fail(error, "ICodecAPI::SetValue", hr);
}
uint32_t PaddedHeight(const CodecConfig& config) { return (config.height + 1u) & ~1u; }
bool SameConfig(const CodecConfig& a, const CodecConfig& b) {
    // The target bitrate is transport metadata, not a decoder format change.
    return a.width == b.width && a.height == b.height && a.fps == b.fps && a.codec == b.codec && a.sequenceHeader == b.sequenceHeader;
}
HRESULT VideoType(ComPtr<IMFMediaType>& type, const CodecConfig& c, const GUID& subtype) {
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return hr;
    if (FAILED(hr = type->SetGUID(MF_MT_SUBTYPE, subtype))) return hr;
    if (FAILED(hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, c.width, PaddedHeight(c)))) return hr;
    if (FAILED(hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, c.fps, 1))) return hr;
    if (FAILED(hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) return hr;
    if (FAILED(hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) return hr;
    if (FAILED(hr = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709))) return hr;
    if (FAILED(hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235))) return hr;
    if (FAILED(hr = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709))) return hr;
    return type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB);
}
HRESULT Sample(const uint8_t* bytes, DWORD length, LONGLONG time, LONGLONG duration, ComPtr<IMFSample>& sample) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(length, &buffer);
    if (FAILED(hr)) return hr;
    BYTE* target{};
    if (FAILED(hr = buffer->Lock(&target, nullptr, nullptr))) return hr;
    std::memcpy(target, bytes, length);
    buffer->Unlock();
    if (FAILED(hr = buffer->SetCurrentLength(length))) return hr;
    if (FAILED(hr = MFCreateSample(&sample))) return hr;
    if (FAILED(hr = sample->AddBuffer(buffer.Get()))) return hr;
    if (FAILED(hr = sample->SetSampleTime(time))) return hr;
    return sample->SetSampleDuration(duration);
}
HRESULT Start(IMFTransform* mft) {
    HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    return FAILED(hr) ? hr : mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}
// Caller owns returned sample; supplied output is released by ComPtr, including
// NEED_MORE_INPUT and STREAM_CHANGE paths. Both MFT allocation modes are handled.
HRESULT Output(IMFTransform* mft, ComPtr<IMFSample>& result) {
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = mft->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;
    ComPtr<IMFSample> supplied;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        if (!info.cbSize || info.cbSize > MaxFrameBytes) return E_INVALIDARG;
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = MFCreateSample(&supplied))) return hr;
        if (FAILED(hr = supplied->AddBuffer(buffer.Get()))) return hr;
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.pSample = supplied.Get();
    DWORD status{};
    hr = mft->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (output.pSample && output.pSample != supplied.Get()) result.Attach(output.pSample);
    else result = supplied;
    return hr;
}
uint8_t Clamp(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }
void BgraToNv12(const Frame& frame, std::vector<uint8_t>& nv12) {
    const uint32_t w = frame.width, h = (frame.height + 1u) & ~1u;
    nv12.resize(size_t(w) * h * 3 / 2);
    for (uint32_t y = 0; y < h; ++y) {
        const auto* row = frame.bgra.data() + size_t(std::min(y, frame.height - 1)) * frame.stride;
        auto* dst = nv12.data() + size_t(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const int b = row[4 * x], g = row[4 * x + 1], r = row[4 * x + 2];
            dst[x] = Clamp(16 + ((11966 * r + 40254 * g + 4064 * b + 32768) >> 16));
        }
    }
    auto* uv = nv12.data() + size_t(w) * h;
    for (uint32_t y = 0; y < h; y += 2) {
        const auto* row0 = frame.bgra.data() + size_t(std::min(y, frame.height - 1)) * frame.stride;
        const auto* row1 = frame.bgra.data() + size_t(std::min(y + 1, frame.height - 1)) * frame.stride;
        for (uint32_t x = 0; x < w; x += 2) {
            int r = 0, g = 0, b = 0;
            for (const auto* row : { row0, row1 }) {
                b += row[4 * x] + row[4 * (x + 1)];
                g += row[4 * x + 1] + row[4 * (x + 1) + 1];
                r += row[4 * x + 2] + row[4 * (x + 1) + 2];
            }
            uv[size_t(y / 2) * w + x] = Clamp(128 + ((-6596 * r - 22189 * g + 28785 * b + 131072) >> 18));
            uv[size_t(y / 2) * w + x + 1] = Clamp(128 + ((28785 * r - 26145 * g - 2640 * b + 131072) >> 18));
        }
    }
}
void Nv12ToBgra(const uint8_t* src, uint32_t stride, uint32_t storageHeight, Frame& frame) {
    const auto* uv = src + size_t(stride) * storageHeight;
    frame.stride = frame.width * 4;
    frame.bgra.resize(size_t(frame.stride) * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto* dst = frame.bgra.data() + size_t(y) * frame.stride;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const int l = int(src[size_t(y) * stride + x]) - 16;
            const int u = int(uv[size_t(y / 2) * stride + (x & ~1u)]) - 128;
            const int v = int(uv[size_t(y / 2) * stride + (x & ~1u) + 1]) - 128;
            dst[4 * x] = Clamp((76309 * l + 138438 * u + 32768) >> 16);
            dst[4 * x + 1] = Clamp((76309 * l - 13975 * u - 34925 * v + 32768) >> 16);
            dst[4 * x + 2] = Clamp((76309 * l + 117489 * v + 32768) >> 16);
            dst[4 * x + 3] = 255;
        }
    }
}
struct Metadata { uint64_t sequence, timestamp100ns; ContentKind kind; uint32_t displayWidth, displayHeight; };
}

bool ValidConfig(const CodecConfig& c) {
    // Inbox H.264 decoder's documented Windows 8+ envelope. Individual encoder
    // configurations can still be rejected by the actual installed MFT.
    return CodecBit(c.codec) && c.width >= 48 && !(c.width & 1) && c.width <= 4096 &&
        c.height >= 48 && c.height <= 2304 && c.fps >= 1 && c.fps <= 240 &&
        c.bitrate >= 64000 && c.bitrate <= 1000000000 && c.sequenceHeader.size() <= MaxSequenceHeaderBytes;
}
const char* CodecName(CodecId codec) {
    return codec == CodecId::Av1 ? "AV1" : codec == CodecId::Hevc ? "HEVC" : codec == CodecId::H264 ? "H.264" : "Unknown";
}
uint32_t HardwareEncodeCapabilities() { return NvEncoder::Probe(); }
uint32_t HardwareDecodeCapabilities() { return NvDecoder::Probe(); }
bool ValidPacket(const EncodedFrame& f) {
    return ValidConfig(f.config) && !f.data.empty() && f.data.size() <= MaxEncodedBytes &&
        (f.kind == ContentKind::Desktop || f.kind == ContentKind::LeiaSbs) &&
        ValidDisplaySize(f.displayWidth, f.displayHeight);
}
struct Encoder::Impl {
    std::unique_ptr<NvEncoder> nv;
    Runtime runtime;
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> codec;
    CodecConfig config;
    LONGLONG nextTime{};
    std::map<LONGLONG, Metadata> pending;
    std::vector<uint8_t> nv12;
    bool ReadHeader(std::string& error) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = mft->GetOutputCurrentType(0, &type);
        if (FAILED(hr)) return Fail(error, "encoder output type", hr);
        UINT32 size{};
        hr = type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size);
        if (hr == MF_E_ATTRIBUTENOTFOUND) return true;
        if (FAILED(hr)) return Fail(error, "encoder sequence header size", hr);
        if (size > MaxSequenceHeaderBytes) { error = "Encoder sequence header exceeds protocol limit"; return false; }
        config.sequenceHeader.resize(size);
        return !size || SUCCEEDED(hr = type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, config.sequenceHeader.data(), size, nullptr)) || Fail(error, "encoder sequence header", hr);
    }
    bool Pull(std::vector<EncodedFrame>& packets, std::string& error) {
        for (unsigned count = 0; count < 64; ++count) {
            ComPtr<IMFSample> sample;
            HRESULT hr = Output(mft.Get(), sample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(hr)) return Fail(error, "encoder ProcessOutput", hr);
            if (!sample) { error = "Encoder returned no sample"; return false; }
            LONGLONG time{};
            if (FAILED(hr = sample->GetSampleTime(&time))) return Fail(error, "encoder sample timestamp", hr);
            const auto it = pending.find(time);
            if (it == pending.end()) { error = "Encoder output timestamp did not match an input frame"; return false; }
            EncodedFrame packet;
            packet.sequence = it->second.sequence;
            packet.timestamp100ns = it->second.timestamp100ns;
            packet.kind = it->second.kind;
            packet.displayWidth = it->second.displayWidth; packet.displayHeight = it->second.displayHeight;
            pending.erase(it);
            if (!ReadHeader(error)) return false;
            packet.config = config;
            UINT32 clean{};
            sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            packet.keyframe = clean != 0;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) return Fail(error, "encoder output buffer", hr);
            BYTE* bytes{}; DWORD size{};
            if (FAILED(hr = buffer->Lock(&bytes, nullptr, &size))) return Fail(error, "encoder output lock", hr);
            if (!size || size > MaxEncodedBytes) { buffer->Unlock(); error = "Encoder packet exceeds protocol limit"; return false; }
            packet.data.assign(bytes, bytes + size);
            buffer->Unlock();
            packets.push_back(std::move(packet));
        }
        error = "Encoder produced excessive output";
        return false;
    }
};
Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}
Encoder::~Encoder() = default;
const CodecConfig& Encoder::config() const { return impl_->nv ? impl_->nv->config() : impl_->config; }
bool Encoder::hardware() const { return impl_->nv != nullptr; }
bool Encoder::Configure(const CodecConfig& config, bool hardware, std::string& error) {
    if (!hardware) {
        if (config.codec != CodecId::H264) { error = "Software fallback supports H.264 only"; return false; }
        return Configure(config.width, config.height, config.fps, config.bitrate, error);
    }
    auto& p = *impl_;
    p.nv.reset(); p.mft.Reset(); p.codec.Reset(); p.pending.clear();
    auto encoder = std::make_unique<NvEncoder>();
    if (!encoder->Configure(config, error)) return false;
    p.config = config;
    p.nv = std::move(encoder);
    return true;
}
bool Encoder::Configure(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate, std::string& error) {
    error.clear();
    auto& p = *impl_;
    p.nv.reset(); p.mft.Reset(); p.codec.Reset(); p.pending.clear(); p.nextTime = 0;
    p.config = { w, h, fps, bitrate, {} };
    if (!ValidConfig(p.config)) { error = "H.264 requires even width 48..4096, height 48..2304, fps 1..240, bitrate 64k..1000M"; return false; }
    if (FAILED(p.runtime.mf)) return Fail(error, "Media Foundation startup", p.runtime.mf);
    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p.mft));
    if (FAILED(hr)) return Fail(error, "create Windows software H.264 encoder", hr);
    if (FAILED(hr = p.mft.As(&p.codec))) return Fail(error, "encoder ICodecAPI", hr);
    if (!SetCodec(p.codec.Get(), CODECAPI_AVLowLatencyMode, VT_BOOL, 1, error) ||
        !SetCodec(p.codec.Get(), CODECAPI_AVEncCommonRateControlMode, VT_UI4, eAVEncCommonRateControlMode_CBR, error) ||
        !SetCodec(p.codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0, error) ||
        !SetCodec(p.codec.Get(), CODECAPI_AVEncMPVGOPSize, VT_UI4, fps, error) ||
        !SetCodec(p.codec.Get(), CODECAPI_AVEncCommonQualityVsSpeed, VT_UI4, 0, error)) return false;
    ComPtr<IMFMediaType> output, input;
    if (FAILED(hr = VideoType(output, p.config, MFVideoFormat_H264))) return Fail(error, "H.264 media type", hr);
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    if (FAILED(hr = p.mft->SetOutputType(0, output.Get(), 0))) return Fail(error, "encoder SetOutputType (try smaller SBS dimensions or lower fps)", hr);
    if (FAILED(hr = VideoType(input, p.config, MFVideoFormat_NV12))) return Fail(error, "NV12 media type", hr);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    input->SetUINT32(MF_MT_SAMPLE_SIZE, w * PaddedHeight(p.config) * 3 / 2);
    if (FAILED(hr = p.mft->SetInputType(0, input.Get(), 0))) return Fail(error, "encoder SetInputType", hr);
    if (!p.ReadHeader(error)) return false;
    if (FAILED(hr = Start(p.mft.Get()))) return Fail(error, "encoder start", hr);
    return RequestKeyFrame(error);
}
bool Encoder::ConfigureGpu(const CodecConfig& config, const GpuFrame& input, std::string& error, const LUID* adapter) {
    auto& p = *impl_;
    p.nv.reset(); p.mft.Reset(); p.codec.Reset(); p.pending.clear();
    auto encoder = std::make_unique<NvEncoder>();
    if (!encoder->ConfigureGpu(config, input, error, adapter)) return false;
    p.config = config;
    p.nv = std::move(encoder);
    return true;
}
bool Encoder::EncodeGpu(const GpuFrame& frame, std::vector<EncodedFrame>& packets, std::string& error) {
    error.clear(); packets.clear();
    if (!impl_->nv) { error = "Shared GPU input requires a configured NVIDIA encoder"; return false; }
    return impl_->nv->EncodeGpu(frame, packets, error);
}
bool Encoder::GpuPreview(Frame& thumbnail) { return impl_->nv && impl_->nv->GpuPreview(thumbnail); }
EncodeTiming Encoder::lastTiming() const { return impl_->nv ? impl_->nv->lastTiming() : EncodeTiming{}; }
bool Encoder::RequestKeyFrame(std::string& error) {
    error.clear();
    if (impl_->nv) return impl_->nv->RequestKeyFrame(error);
    if (!impl_->codec) { error = "Encoder is not configured"; return false; }
    return SetCodec(impl_->codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, VT_UI4, 1, error);
}
bool Encoder::UpdateBitrate(uint32_t bitrate, std::string& error) {
    error.clear();
    if (impl_->nv) return impl_->nv->UpdateBitrate(bitrate, error);
    error = "Dynamic bitrate adaptation requires NVENC; software fallback retains its configured target";
    return false;
}
bool Encoder::Encode(const Frame& frame, std::vector<EncodedFrame>& packets, std::string& error) {
    error.clear(); packets.clear();
    auto& p = *impl_;
    if (p.nv) return p.nv->Encode(frame, packets, error);
    if (!p.mft || !ValidFrame(frame) || frame.width != p.config.width || frame.height != p.config.height) { error = "Frame does not match the configured encoder"; return false; }
    if (p.pending.size() >= 64) { error = "Encoder pending frame limit exceeded"; return false; }
    BgraToNv12(frame, p.nv12);
    const LONGLONG duration = 10000000 / p.config.fps;
    ComPtr<IMFSample> sample;
    HRESULT hr = Sample(p.nv12.data(), static_cast<DWORD>(p.nv12.size()), p.nextTime, duration, sample);
    if (FAILED(hr)) return Fail(error, "encoder input sample", hr);
    if (p.nextTime == 0) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    hr = p.mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!p.Pull(packets, error)) return false;
        hr = p.mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return Fail(error, "encoder ProcessInput", hr);
    p.pending.emplace(p.nextTime, Metadata{ frame.sequence, frame.timestamp100ns, frame.kind, frame.displayWidth, frame.displayHeight });
    p.nextTime += duration;
    return p.Pull(packets, error);
}

struct Decoder::Impl {
    std::unique_ptr<NvDecoder> nv;
    bool preferHardware{}, gpuOutput{}; LUID outputAdapter{};
    Runtime runtime;
    ComPtr<IMFTransform> mft;
    CodecConfig config;
    uint32_t outputWidth{}, outputHeight{}, stride{};
    LONGLONG nextTime{};
    bool started{};
    std::map<LONGLONG, Metadata> pending;
    bool SetOutput(std::string& error) {
        for (DWORD index = 0; index < 32; ++index) {
            ComPtr<IMFMediaType> type;
            HRESULT hr = mft->GetOutputAvailableType(0, index, &type);
            if (FAILED(hr)) return Fail(error, "decoder enumerate output", hr);
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
            if (FAILED(hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &outputWidth, &outputHeight))) return Fail(error, "decoder dimensions", hr);
            if (outputWidth < config.width || outputHeight < PaddedHeight(config) ||
                outputWidth > ((config.width + 15u) & ~15u) || outputHeight > ((PaddedHeight(config) + 15u) & ~15u)) { error = "Decoded dimensions do not match the announced stream"; return false; }
            stride = MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, outputWidth);
            if (stride < outputWidth || uint64_t(stride) * outputHeight * 3 / 2 > MaxFrameBytes) { error = "Invalid decoder NV12 stride"; return false; }
            if (FAILED(hr = mft->SetOutputType(0, type.Get(), 0))) return Fail(error, "decoder SetOutputType", hr);
            return true;
        }
        error = "Decoder does not offer NV12";
        return false;
    }
    bool Pull(std::vector<Frame>& frames, std::string& error) {
        for (unsigned count = 0; count < 64; ++count) {
            ComPtr<IMFSample> sample;
            HRESULT hr = Output(mft.Get(), sample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!SetOutput(error)) return false; continue; }
            if (FAILED(hr)) return Fail(error, "decoder ProcessOutput", hr);
            if (!sample) { error = "Decoder returned no sample"; return false; }
            LONGLONG time{};
            if (FAILED(hr = sample->GetSampleTime(&time))) return Fail(error, "decoder sample timestamp", hr);
            const auto it = pending.find(time);
            if (it == pending.end()) { error = "Decoder output timestamp did not match an input packet"; return false; }
            Frame frame;
            frame.width = config.width; frame.height = config.height;
            frame.sequence = it->second.sequence; frame.timestamp100ns = it->second.timestamp100ns;
            frame.kind = it->second.kind;
            frame.displayWidth = it->second.displayWidth; frame.displayHeight = it->second.displayHeight;
            pending.erase(it);
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) return Fail(error, "decoder output buffer", hr);
            BYTE* bytes{}; DWORD size{};
            if (FAILED(hr = buffer->Lock(&bytes, nullptr, &size))) return Fail(error, "decoder output lock", hr);
            const uint64_t required = uint64_t(stride) * outputHeight * 3 / 2;
            if (size < required) { buffer->Unlock(); error = "Truncated decoded NV12 image"; return false; }
            Nv12ToBgra(bytes, stride, outputHeight, frame);
            buffer->Unlock();
            frames.push_back(std::move(frame));
        }
        error = "Decoder produced excessive output";
        return false;
    }
};
Decoder::Decoder(bool preferHardware, const LUID* adapter) : impl_(std::make_unique<Impl>()) { impl_->preferHardware = preferHardware; impl_->gpuOutput = adapter != nullptr; if (adapter) impl_->outputAdapter = *adapter; }
Decoder::~Decoder() = default;
bool Decoder::hardware() const { return impl_->nv != nullptr; }
bool Decoder::Configure(const CodecConfig& config, std::string& error) {
    error.clear();
    auto& p = *impl_;
    p.nv.reset(); p.mft.Reset(); p.pending.clear(); p.nextTime = 0; p.started = false; p.config = config;
    if (!ValidConfig(config)) { error = "Invalid video stream configuration"; return false; }
    if (p.preferHardware || config.codec != CodecId::H264) {
        auto decoder = std::make_unique<NvDecoder>(p.gpuOutput ? &p.outputAdapter : nullptr);
        if (decoder->Configure(config, error)) { p.nv = std::move(decoder); return true; }
        if (config.codec != CodecId::H264 || p.gpuOutput) return false;
        error.clear(); // H.264 retains the Windows software fallback.
    }
    if (FAILED(p.runtime.mf)) return Fail(error, "Media Foundation startup", p.runtime.mf);
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p.mft));
    if (FAILED(hr)) return Fail(error, "create Windows H.264 decoder", hr);
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(p.mft->GetAttributes(&attributes))) {
        attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, FALSE);
        attributes->SetUINT32(CODECAPI_AVDecVideoMaxCodedWidth, 4096);
        attributes->SetUINT32(CODECAPI_AVDecVideoMaxCodedHeight, 2304);
    }
    ComPtr<IMFMediaType> input;
    if (FAILED(hr = VideoType(input, config, MFVideoFormat_H264))) return Fail(error, "decoder input type", hr);
    if (!config.sequenceHeader.empty()) input->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, config.sequenceHeader.data(), static_cast<UINT32>(config.sequenceHeader.size()));
    if (FAILED(hr = p.mft->SetInputType(0, input.Get(), 0))) return Fail(error, "decoder SetInputType", hr);
    if (!p.SetOutput(error)) return false;
    if (FAILED(hr = Start(p.mft.Get()))) return Fail(error, "decoder start", hr);
    return true;
}
bool Decoder::Decode(const EncodedFrame& packet, std::vector<Frame>& frames, std::string& error) {
    error.clear(); frames.clear();
    auto& p = *impl_;
    if (!ValidPacket(packet)) { error = "Invalid video packet"; return false; }
    if ((!p.mft && !p.nv) || !SameConfig(p.config, packet.config)) {
        if (!packet.keyframe) { error = "A new video stream must start with a keyframe"; return false; }
        if (!Configure(packet.config, error)) return false;
    }
    p.config.bitrate = packet.config.bitrate;
    if (p.nv) return p.nv->Decode(packet, frames, error);
    if (!p.started && !packet.keyframe) { error = "Decoder is waiting for a keyframe"; return false; }
    if (p.pending.size() >= 64) { error = "Decoder pending frame limit exceeded"; return false; }
    const LONGLONG duration = 10000000 / p.config.fps;
    ComPtr<IMFSample> sample;
    // Annex B parameter sets are also prefixed to the first IDR because the
    // inbox decoder expects SPS/PPS in its input elementary stream.
    std::vector<uint8_t> prefixed;
    const uint8_t* bytes = packet.data.data();
    size_t size = packet.data.size();
    if (!p.started && !p.config.sequenceHeader.empty()) {
        prefixed = p.config.sequenceHeader;
        prefixed.insert(prefixed.end(), packet.data.begin(), packet.data.end());
        bytes = prefixed.data(); size = prefixed.size();
    }
    HRESULT hr = Sample(bytes, static_cast<DWORD>(size), p.nextTime, duration, sample);
    if (FAILED(hr)) return Fail(error, "decoder input sample", hr);
    sample->SetUINT32(MFSampleExtension_CleanPoint, packet.keyframe);
    if (!p.started) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    hr = p.mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!p.Pull(frames, error)) return false;
        hr = p.mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return Fail(error, "decoder ProcessInput", hr);
    p.pending.emplace(p.nextTime, Metadata{ packet.sequence, packet.timestamp100ns, packet.kind, packet.displayWidth, packet.displayHeight });
    p.nextTime += duration; p.started = true;
    return p.Pull(frames, error);
}
}
