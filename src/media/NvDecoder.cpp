#include "media/NvDecoder.h"
#include "media/DecodedGpuOutput.h"
#include <Windows.h>
#include "../../third_party/nvcodec/dynlink_cuda.h"
#include "../../third_party/nvcodec/dynlink_nvcuvid.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <new>

namespace lan3d::media {
namespace {
// PTX is JIT compiled by the display driver, so CUDA SDK/nvcc is not required.
// Each thread converts one NV12 pixel using limited-range BT.709 coefficients.
// Integer coefficients are rounded to 8 fractional bits (at most ~1 LSB error).
constexpr char ConvertPtx[] = R"ptx(
.version 6.0
.target sm_50
.address_size 64
.visible .entry nv12_bgra(
    .param .u64 source, .param .u64 target, .param .u32 pitch,
    .param .u32 width, .param .u32 height, .param .u32 surfaceHeight)
{
    .reg .pred p<3>;
    .reg .b32 r<35>;
    .reg .b64 d<10>;
    ld.param.u64 d0, [source];
    ld.param.u64 d1, [target];
    ld.param.u32 r0, [pitch];
    ld.param.u32 r1, [width];
    ld.param.u32 r2, [height];
    ld.param.u32 r3, [surfaceHeight];
    mov.u32 r4, %ctaid.x;
    mov.u32 r5, %ntid.x;
    mov.u32 r6, %tid.x;
    mad.lo.u32 r7, r4, r5, r6;
    mov.u32 r4, %ctaid.y;
    mov.u32 r5, %ntid.y;
    mov.u32 r6, %tid.y;
    mad.lo.u32 r8, r4, r5, r6;
    setp.ge.u32 p0, r7, r1;
    setp.ge.u32 p1, r8, r2;
    or.pred p2, p0, p1;
    @p2 bra done;
    mad.lo.u32 r9, r8, r0, r7;
    cvt.u64.u32 d2, r9;
    add.u64 d3, d0, d2;
    ld.global.u8 r10, [d3];
    shr.u32 r11, r8, 1;
    and.b32 r12, r7, 0xfffffffe;
    mad.lo.u32 r13, r3, r0, r12;
    mad.lo.u32 r13, r11, r0, r13;
    cvt.u64.u32 d2, r13;
    add.u64 d3, d0, d2;
    ld.global.u8 r14, [d3];
    ld.global.u8 r15, [d3+1];
    sub.s32 r10, r10, 16;
    sub.s32 r14, r14, 128;
    sub.s32 r15, r15, 128;
    mul.lo.s32 r16, r10, 298;
    add.s32 r16, r16, 128;
    mad.lo.s32 r17, r14, 541, r16;
    mad.lo.s32 r18, r14, -55, r16;
    mad.lo.s32 r18, r15, -136, r18;
    mad.lo.s32 r19, r15, 459, r16;
    shr.s32 r17, r17, 8;
    shr.s32 r18, r18, 8;
    shr.s32 r19, r19, 8;
    max.s32 r17, r17, 0;
    max.s32 r18, r18, 0;
    max.s32 r19, r19, 0;
    min.s32 r17, r17, 255;
    min.s32 r18, r18, 255;
    min.s32 r19, r19, 255;
    shl.b32 r18, r18, 8;
    shl.b32 r19, r19, 16;
    or.b32 r20, r17, r18;
    or.b32 r20, r20, r19;
    or.b32 r20, r20, 0xff000000;
    mad.lo.u32 r21, r8, r1, r7;
    mul.wide.u32 d4, r21, 4;
    add.u64 d5, d1, d4;
    st.global.u32 [d5], r20;
done:
    ret;
}
)ptx";

bool CudaError(std::string& error, const char* operation, CUresult result) {
    char text[192];
    std::snprintf(text, sizeof(text), "%s failed (CUDA %d)", operation, int(result));
    error = text;
    return false;
}

struct Driver {
    HMODULE cuda{}, video{};
#define CUDA_FUNCTIONS(X) \
    X(cuInit) X(cuDeviceGetCount) X(cuDeviceGet) X(cuCtxCreate_v2) \
    X(cuCtxPushCurrent_v2) X(cuCtxPopCurrent_v2) X(cuCtxDestroy_v2) \
    X(cuMemAlloc_v2) X(cuMemFree_v2) X(cuMemcpyDtoH_v2) X(cuDeviceGetLuid) X(cuMemcpy2D_v2) \
    X(cuGraphicsD3D11RegisterResource) X(cuGraphicsUnregisterResource) X(cuGraphicsMapResources) \
    X(cuGraphicsUnmapResources) X(cuGraphicsResourceGetMappedPointer) \
    X(cuModuleLoadData) X(cuModuleUnload) X(cuModuleGetFunction) X(cuLaunchKernel)
#define VIDEO_FUNCTIONS(X) \
    X(cuvidGetDecoderCaps) X(cuvidCreateDecoder) X(cuvidDestroyDecoder) \
    X(cuvidDecodePicture) X(cuvidMapVideoFrame64) X(cuvidUnmapVideoFrame64) \
    X(cuvidCreateVideoParser) X(cuvidDestroyVideoParser) X(cuvidParseVideoData)
#define DECLARE_FUNCTION(name) t##name* name{};
    CUDA_FUNCTIONS(DECLARE_FUNCTION)
    VIDEO_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION
    ~Driver() {
        if (video) FreeLibrary(video);
        if (cuda) FreeLibrary(cuda);
    }
    bool Load(std::string& error) {
        cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        video = LoadLibraryExW(L"nvcuvid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!cuda || !video) { error = "NVIDIA CUDA/NVDEC display driver is unavailable"; return false; }
#define LOAD_CUDA(name) name = reinterpret_cast<t##name*>(GetProcAddress(cuda, #name)); if (!name) { error = "Missing CUDA export: " #name; return false; }
#define LOAD_VIDEO(name) name = reinterpret_cast<t##name*>(GetProcAddress(video, #name)); if (!name) { error = "Missing NVDEC export: " #name; return false; }
        CUDA_FUNCTIONS(LOAD_CUDA)
        // The unsuffixed export is the legacy 32-bit pointer/size ABI.
        cuGraphicsResourceGetMappedPointer = reinterpret_cast<tcuGraphicsResourceGetMappedPointer*>(GetProcAddress(cuda, "cuGraphicsResourceGetMappedPointer_v2"));
        if (!cuGraphicsResourceGetMappedPointer) { error = "Missing CUDA mapped pointer v2 export"; return false; }
        VIDEO_FUNCTIONS(LOAD_VIDEO)
#undef LOAD_CUDA
#undef LOAD_VIDEO
        const auto result = cuInit(0);
        return result == CUDA_SUCCESS || CudaError(error, "cuInit", result);
    }
};
#undef CUDA_FUNCTIONS
#undef VIDEO_FUNCTIONS

struct CurrentContext {
    Driver& api;
    CUresult status;
    CurrentContext(Driver& driver, CUcontext context) : api(driver), status(api.cuCtxPushCurrent_v2(context)) {}
    ~CurrentContext() { if (status == CUDA_SUCCESS) { CUcontext popped{}; api.cuCtxPopCurrent_v2(&popped); } }
};

struct MappedFrame {
    Driver& api;
    CUvideodecoder decoder;
    CUdeviceptr pointer{};
    ~MappedFrame() { if (pointer) api.cuvidUnmapVideoFrame64(decoder, pointer); }
    CUresult Unmap() {
        const auto result = api.cuvidUnmapVideoFrame64(decoder, pointer);
        pointer = 0;
        return result;
    }
};

cudaVideoCodec VideoCodec(CodecId codec) {
    switch (codec) {
    case CodecId::H264: return cudaVideoCodec_H264;
    case CodecId::Hevc: return cudaVideoCodec_HEVC;
    case CodecId::Av1: return cudaVideoCodec_AV1;
    default: return cudaVideoCodec_NumCodecs;
    }
}
bool Supports(Driver& api, CodecId codec, uint32_t width, uint32_t height, CUVIDDECODECAPS* output = nullptr) {
    CUVIDDECODECAPS caps{};
    caps.eCodecType = VideoCodec(codec);
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    if (api.cuvidGetDecoderCaps(&caps) != CUDA_SUCCESS || !caps.bIsSupported ||
        !(caps.nOutputFormatMask & (1u << cudaVideoSurfaceFormat_NV12))) return false;
    if (output) *output = caps;
    return width >= caps.nMinWidth && width <= caps.nMaxWidth &&
        height >= caps.nMinHeight && height <= caps.nMaxHeight &&
        uint64_t((width + 15) / 16) * ((height + 15) / 16) <= caps.nMaxMBCount;
}
bool SameConfig(const CodecConfig& a, const CodecConfig& b) {
    return a.width == b.width && a.height == b.height && a.fps == b.fps &&
        a.codec == b.codec && a.sequenceHeader == b.sequenceHeader;
}
}

struct NvDecoder::Impl {
    Driver api;
    CUcontext context{};
    CUvideoparser parser{};
    CUvideodecoder decoder{};
    CUmodule module{};
    CUfunction convert{};
    CUdeviceptr bgra{};
    std::unique_ptr<DecodedGpuOutput> gpuOutput;
    CUgraphicsResource gpuInput{};
    CodecConfig config;
    uint32_t codedWidth{}, codedHeight{}, surfaces{};
    uint64_t nextToken{1};
    bool started{}, failed{}, callbackException{};
    std::string callbackError;
    struct Metadata { uint64_t sequence, time; ContentKind kind; uint32_t displayWidth, displayHeight; };
    std::map<uint64_t, Metadata> pending;
    std::vector<Frame>* output{};

    ~Impl() {
        if (!context) return;
        {
            CurrentContext current(api, context);
            if (current.status == CUDA_SUCCESS) {
                // No EOS flush: teardown must not invoke callbacks into a released output vector.
                if (parser) api.cuvidDestroyVideoParser(parser);
                if (decoder) api.cuvidDestroyDecoder(decoder);
                if (gpuInput) api.cuGraphicsUnregisterResource(gpuInput);
                if (bgra) api.cuMemFree_v2(bgra);
                if (module) api.cuModuleUnload(module);
            }
        }
        api.cuCtxDestroy_v2(context);
    }
    bool Init(const CodecConfig& requested, std::string& error, const LUID* adapter) {
        config = requested;
        if (!api.Load(error)) return false;
        int count{};
        auto result = api.cuDeviceGetCount(&count);
        if (result != CUDA_SUCCESS) return CudaError(error, "cuDeviceGetCount", result);
        for (int index = 0; index < count; ++index) {
            CUdevice device{};
            if (api.cuDeviceGet(&device, index) != CUDA_SUCCESS) continue;
            if (adapter) {
                LUID actual{}; unsigned mask{};
                if (api.cuDeviceGetLuid(reinterpret_cast<char*>(&actual), &mask, device) != CUDA_SUCCESS ||
                    actual.HighPart != adapter->HighPart || actual.LowPart != adapter->LowPart) continue;
            }
            CUcontext candidate{};
            if (api.cuCtxCreate_v2(&candidate, CU_CTX_SCHED_BLOCKING_SYNC, device) != CUDA_SUCCESS) continue;
            const bool supported = Supports(api, config.codec, config.width, (config.height + 1u) & ~1u);
            CUcontext popped{};
            api.cuCtxPopCurrent_v2(&popped);
            if (supported) { context = candidate; break; }
            api.cuCtxDestroy_v2(candidate);
        }
        if (!context) { error = "No NVIDIA GPU supports this codec and image size"; return false; }
        CurrentContext current(api, context);
        if (current.status != CUDA_SUCCESS) return CudaError(error, "cuCtxPushCurrent", current.status);
        if (adapter) {
            gpuOutput = std::make_unique<DecodedGpuOutput>();
            if (!gpuOutput->Open(*adapter, config.width, config.height, error)) return false;
            result = api.cuGraphicsD3D11RegisterResource(&gpuInput, gpuOutput->Input(), 0);
            if (result != CUDA_SUCCESS) return CudaError(error, "NVDEC D3D11 input registration", result);
        } else {
        if ((result = api.cuModuleLoadData(&module, ConvertPtx)) != CUDA_SUCCESS) return CudaError(error, "CUDA color converter JIT", result);
        if ((result = api.cuModuleGetFunction(&convert, module, "nv12_bgra")) != CUDA_SUCCESS) return CudaError(error, "cuModuleGetFunction", result);
        if ((result = api.cuMemAlloc_v2(&bgra, size_t(config.width) * config.height * 4)) != CUDA_SUCCESS) return CudaError(error, "cuMemAlloc BGRA", result);
        }
        CUVIDPARSERPARAMS params{};
        params.CodecType = VideoCodec(config.codec);
        params.ulMaxNumDecodeSurfaces = 1;
        params.ulClockRate = 10000000;
        params.ulMaxDisplayDelay = 0;
        params.pUserData = this;
        params.pfnSequenceCallback = SequenceCallback;
        params.pfnDecodePicture = DecodeCallback;
        params.pfnDisplayPicture = DisplayCallback;
        if ((result = api.cuvidCreateVideoParser(&parser, &params)) != CUDA_SUCCESS) return CudaError(error, "cuvidCreateVideoParser", result);
        if (!config.sequenceHeader.empty()) {
            CUVIDSOURCEDATAPACKET header{};
            header.payload = config.sequenceHeader.data();
            header.payload_size = static_cast<unsigned long>(config.sequenceHeader.size());
            result = api.cuvidParseVideoData(parser, &header);
            if (result != CUDA_SUCCESS || callbackException || !callbackError.empty()) {
                if (callbackException) { error = "NVDEC sequence callback failed"; return false; }
                if (!callbackError.empty()) { error = callbackError; return false; }
                return CudaError(error, "NVDEC sequence header", result);
            }
        }
        return true;
    }
    static int CUDAAPI SequenceCallback(void* user, CUVIDEOFORMAT* format) noexcept {
        try { return static_cast<Impl*>(user)->Sequence(*format); }
        catch (...) { static_cast<Impl*>(user)->callbackException = true; return 0; }
    }
    static int CUDAAPI DecodeCallback(void* user, CUVIDPICPARAMS* picture) noexcept {
        auto& self = *static_cast<Impl*>(user);
        try {
            if (!self.decoder) { self.callbackError = "NVDEC picture before sequence"; return 0; }
            const auto result = self.api.cuvidDecodePicture(self.decoder, picture);
            if (result != CUDA_SUCCESS) { CudaError(self.callbackError, "cuvidDecodePicture", result); return 0; }
            return 1;
        } catch (...) { self.callbackException = true; return 0; }
    }
    static int CUDAAPI DisplayCallback(void* user, CUVIDPARSERDISPINFO* display) noexcept {
        try { return display ? static_cast<Impl*>(user)->Display(*display) : 1; }
        catch (...) { static_cast<Impl*>(user)->callbackException = true; return 0; }
    }
    int Sequence(const CUVIDEOFORMAT& format) {
        const auto& signal = format.video_signal_description;
        const uint32_t height = (config.height + 1u) & ~1u;
        const bool primaries = signal.color_primaries == 1 || signal.color_primaries == 2;
        const bool transfer = signal.transfer_characteristics == 1 ||
            signal.transfer_characteristics == 2 || signal.transfer_characteristics == 13;
        const bool matrix = signal.matrix_coefficients == 1 || signal.matrix_coefficients == 2;
        if (format.codec != VideoCodec(config.codec) || !format.progressive_sequence ||
            format.bit_depth_luma_minus8 || format.bit_depth_chroma_minus8 ||
            format.chroma_format != cudaVideoChromaFormat_420 || signal.video_full_range_flag ||
            !primaries || !transfer || !matrix ||
            format.coded_width < config.width || format.coded_width > 4096 ||
            format.coded_height < height || format.coded_height > 2304 ||
            format.display_area.left || format.display_area.top ||
            format.display_area.right != static_cast<int>(config.width) ||
            (format.display_area.bottom != static_cast<int>(height) && format.display_area.bottom != static_cast<int>(config.height)) ||
            !format.min_num_decode_surfaces || format.min_num_decode_surfaces > 32 ||
            !Supports(api, config.codec, format.coded_width, format.coded_height)) {
            callbackError = "NVDEC stream does not match the negotiated 8-bit SDR BT.709 NV12 image";
            return 0;
        }
        const uint32_t required = std::max(2u, uint32_t(format.min_num_decode_surfaces));
        if (decoder) {
            if (codedWidth == format.coded_width && codedHeight == format.coded_height && surfaces >= required) return int(surfaces);
            callbackError = "NVDEC coded size changed without a new stream configuration";
            return 0;
        }
        CUVIDDECODECREATEINFO info{};
        info.CodecType = format.codec;
        info.ChromaFormat = format.chroma_format;
        info.OutputFormat = cudaVideoSurfaceFormat_NV12;
        info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        info.ulWidth = info.ulMaxWidth = format.coded_width;
        info.ulHeight = info.ulMaxHeight = format.coded_height;
        info.ulNumDecodeSurfaces = required;
        info.ulNumOutputSurfaces = 1;
        info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
        info.display_area.right = static_cast<short>(config.width);
        info.display_area.bottom = static_cast<short>(height);
        info.ulTargetWidth = config.width;
        info.ulTargetHeight = height;
        const auto result = api.cuvidCreateDecoder(&decoder, &info);
        if (result != CUDA_SUCCESS) { CudaError(callbackError, "cuvidCreateDecoder", result); return 0; }
        codedWidth = format.coded_width; codedHeight = format.coded_height; surfaces = required;
        return int(required);
    }
    int Display(const CUVIDPARSERDISPINFO& display) {
        const auto metadata = pending.find(static_cast<uint64_t>(display.timestamp));
        if (!output || metadata == pending.end() || !display.progressive_frame) {
            callbackError = "NVDEC output has no matching frame metadata or is interlaced"; return 0;
        }
        Frame frame;
        frame.width = config.width; frame.height = config.height; frame.stride = config.width * 4;
        frame.sequence = metadata->second.sequence; frame.timestamp100ns = metadata->second.time; frame.kind = metadata->second.kind;
        frame.displayWidth = metadata->second.displayWidth; frame.displayHeight = metadata->second.displayHeight;
        if (!gpuOutput) frame.bgra.resize(size_t(frame.stride) * frame.height);
        CUVIDPROCPARAMS params{};
        params.progressive_frame = 1;
        params.top_field_first = display.top_field_first;
        params.unpaired_field = 1;
        MappedFrame mapped{ api, decoder };
        unsigned int pitch{};
        auto result = api.cuvidMapVideoFrame64(decoder, display.picture_index, &mapped.pointer, &pitch, &params);
        if (result != CUDA_SUCCESS) { CudaError(callbackError, "cuvidMapVideoFrame", result); return 0; }
        uint32_t surfaceHeight = (config.height + 1u) & ~1u;
        if (gpuOutput) {
            result = api.cuGraphicsMapResources(1, &gpuInput, nullptr);
            if (result != CUDA_SUCCESS) { CudaError(callbackError, "NVDEC map D3D11 interop", result); return 0; }
            if (result == CUDA_SUCCESS) {
                CUdeviceptr pointer{}; size_t size{};
                result = api.cuGraphicsResourceGetMappedPointer(&pointer, &size, gpuInput);
                if (result == CUDA_SUCCESS && size < size_t(config.width) * surfaceHeight * 3 / 2) result = static_cast<CUresult>(1);
                if (result == CUDA_SUCCESS) {
                    CUDA_MEMCPY2D copy{}; copy.srcMemoryType = CU_MEMORYTYPE_DEVICE; copy.srcDevice = mapped.pointer; copy.srcPitch = pitch;
                    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE; copy.dstDevice = pointer; copy.dstPitch = config.width;
                    copy.WidthInBytes = config.width; copy.Height = surfaceHeight * 3 / 2;
                    result = api.cuMemcpy2D_v2(&copy);
                }
                if (result != CUDA_SUCCESS) CudaError(callbackError, "NVDEC copy NV12 into D3D11", result);
                const auto released = api.cuGraphicsUnmapResources(1, &gpuInput, nullptr);
                if (result == CUDA_SUCCESS) { result = released; if (result != CUDA_SUCCESS) CudaError(callbackError, "NVDEC release D3D11 interop", result); }
            }
        } else {
        void* arguments[] = { &mapped.pointer, &bgra, &pitch, &config.width, &config.height, &surfaceHeight };
        result = api.cuLaunchKernel(convert, (config.width + 15) / 16, (config.height + 15) / 16, 1, 16, 16, 1, 0, nullptr, arguments, nullptr);
        if (result == CUDA_SUCCESS) result = api.cuMemcpyDtoH_v2(frame.bgra.data(), bgra, frame.bgra.size());
        }
        const auto unmapResult = mapped.Unmap();
        if (result != CUDA_SUCCESS) { if (callbackError.empty()) CudaError(callbackError, "NVDEC GPU color conversion/download", result); return 0; }
        if (unmapResult != CUDA_SUCCESS) { CudaError(callbackError, "cuvidUnmapVideoFrame", unmapResult); return 0; }
        if (gpuOutput && !gpuOutput->Convert(frame, callbackError)) return 0;
        if (frame.width) output->push_back(std::move(frame));
        pending.erase(metadata);
        return 1;
    }
};

NvDecoder::NvDecoder(const LUID* adapter) : gpuOutput_(adapter != nullptr), outputAdapter_(adapter ? *adapter : LUID{}) {}
NvDecoder::~NvDecoder() = default;

uint32_t NvDecoder::Probe() {
    Driver api;
    std::string error;
    if (!api.Load(error)) return 0;
    int count{};
    if (api.cuDeviceGetCount(&count) != CUDA_SUCCESS) return 0;
    uint32_t supported{};
    for (int index = 0; index < count && supported != 7; ++index) {
        CUdevice device{};
        CUcontext context{};
        if (api.cuDeviceGet(&device, index) != CUDA_SUCCESS ||
            api.cuCtxCreate_v2(&context, CU_CTX_SCHED_BLOCKING_SYNC, device) != CUDA_SUCCESS) continue;
        for (const auto codec : { CodecId::H264, CodecId::Hevc, CodecId::Av1 })
            if (Supports(api, codec, 3840, 2160)) supported |= CodecBit(codec);
        CUcontext popped{};
        api.cuCtxPopCurrent_v2(&popped);
        api.cuCtxDestroy_v2(context);
    }
    return supported;
}

bool NvDecoder::Configure(const CodecConfig& config, std::string& error) {
    error.clear();
    if (!ValidConfig(config)) { error = "Invalid NVDEC stream configuration"; return false; }
    auto next = std::make_unique<Impl>();
    if (!next->Init(config, error, gpuOutput_ ? &outputAdapter_ : nullptr)) return false;
    impl_ = std::move(next);
    return true;
}

bool NvDecoder::Decode(const EncodedFrame& packet, std::vector<Frame>& frames, std::string& error) {
    frames.clear(); error.clear();
    if (!ValidPacket(packet)) { error = "Invalid NVDEC packet"; return false; }
    if (!impl_ || !SameConfig(impl_->config, packet.config)) {
        if (!packet.keyframe) { error = "NVDEC stream must start with a keyframe"; return false; }
        if (!Configure(packet.config, error)) return false;
    }
    auto& state = *impl_;
    if (state.failed) { error = "NVDEC stream failed; a new configuration is required"; return false; }
    if (!state.started && !packet.keyframe) { error = "NVDEC stream must start with a keyframe"; return false; }
    if (state.pending.size() >= 64) { state.failed = true; error = "NVDEC metadata queue overflow"; return false; }
    CurrentContext current(state.api, state.context);
    if (current.status != CUDA_SUCCESS) return CudaError(error, "cuCtxPushCurrent", current.status);
    const uint64_t token = state.nextToken++;
    state.pending.emplace(token, Impl::Metadata{ packet.sequence, packet.timestamp100ns, packet.kind, packet.displayWidth, packet.displayHeight });
    CUVIDSOURCEDATAPACKET input{};
    input.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    input.payload_size = static_cast<unsigned long>(packet.data.size());
    input.payload = packet.data.data();
    // Parser timestamps are private unique tokens. Original sequence, time, and
    // content kind are restored together, even if timestamps repeat or are zero.
    input.timestamp = static_cast<CUvideotimestamp>(token);
    state.output = &frames;
    state.callbackError.clear();
    state.callbackException = false;
    const auto result = state.api.cuvidParseVideoData(state.parser, &input);
    state.output = nullptr;
    if (result != CUDA_SUCCESS || state.callbackException || !state.callbackError.empty()) {
        state.failed = true; frames.clear();
        if (state.callbackException) { error = "NVDEC callback failed"; return false; }
        if (!state.callbackError.empty()) { error = state.callbackError; return false; }
        return CudaError(error, "cuvidParseVideoData", result);
    }
    state.started = true;
    return true;
}
}
