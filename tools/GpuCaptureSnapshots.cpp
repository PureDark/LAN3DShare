#include "common/FrameBus.h"
#include "common/GpuFrameBus.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace lan3d;
namespace {
void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { char error[160]{}; sprintf_s(error, "%s failed (0x%08X)", what, unsigned(hr)); throw std::runtime_error(error); }
}
struct Options { uint32_t pid{}, frames{3}, intervalMs{2000}; fs::path output; };
Options Parse(int argc, wchar_t** argv) {
    Options result;
    for (int n = 1; n < argc; ++n) {
        const std::wstring option = argv[n];
        if (++n == argc) throw std::runtime_error("Missing argument value");
        if (option == L"--output-dir") { result.output = fs::absolute(argv[n]); continue; }
        size_t used{}; const auto value = std::stoull(argv[n], &used);
        if (used != wcslen(argv[n]) || value > UINT32_MAX) throw std::runtime_error("Invalid numeric argument");
        if (option == L"--pid") result.pid = uint32_t(value);
        else if (option == L"--frames") result.frames = uint32_t(value);
        else if (option == L"--interval-ms") result.intervalMs = uint32_t(value);
        else throw std::runtime_error("Unknown argument");
    }
    if (!result.pid || result.output.empty() || !result.frames || result.frames > 3 || result.intervalMs < 500 ||
        uint64_t(result.frames - 1) * result.intervalMs > 12000)
        throw std::runtime_error("Require --pid, --output-dir, frames 1..3 and interval >=500 ms; total interval <=12 s");
    return result;
}
struct Lease {
    GpuFrameBus& bus;
    uint64_t sequence;
    bool submitted{}, completed{}, released{};
    bool Release() {
        if (released) return true;
        if (submitted && !completed) return false;
        for (unsigned n = 0; n < 100; ++n) {
            if (bus.Release(sequence)) { released = true; return true; }
            Sleep(1);
        }
        return false;
    }
    ~Lease() {
        if (!Release()) std::cerr << "GPU_LEASE_QUARANTINED sequence=" << sequence
            << " gpu_submitted=" << submitted << " gpu_completed=" << completed << '\n' << std::flush;
    }
};
class Reader {
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    LUID luid{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{}, height{};
public:
    Frame Read(const GpuFrame& input, Lease& lease) {
        if (!ValidGpuFrame(input)) throw std::runtime_error("Invalid GPU mailbox metadata");
        if (!device || luid.LowPart != input.adapterLuid.LowPart || luid.HighPart != input.adapterLuid.HighPart) {
            staging.Reset(); context.Reset(); device.Reset();
            ComPtr<IDXGIFactory1> factory;
            Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
            for (UINT n = 0;; ++n) {
                ComPtr<IDXGIAdapter1> adapter;
                const auto found = factory->EnumAdapters1(n, &adapter);
                if (found == DXGI_ERROR_NOT_FOUND) break;
                Check(found, "enumerate source adapter");
                DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc), "source adapter identity");
                if (desc.AdapterLuid.LowPart != input.adapterLuid.LowPart || desc.AdapterLuid.HighPart != input.adapterLuid.HighPart) continue;
                ComPtr<ID3D11Device> base;
                const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &base, nullptr, &context), "snapshot D3D11 device");
                Check(base.As(&device), "shared texture device interface");
                luid = input.adapterLuid; break;
            }
            if (!device) throw std::runtime_error("Source adapter LUID not found");
        }
        ComPtr<ID3D11Texture2D> source;
        Check(device->OpenSharedResourceByName(input.textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&source)), "open source shared texture");
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        if (desc.Width != input.width || desc.Height != input.height || desc.Format != input.format ||
            desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
            throw std::runtime_error("Shared source texture differs from mailbox metadata");
        if (!staging || width != input.width || height != input.height || format != input.format) {
            staging.Reset(); desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = desc.MiscFlags = 0;
            Check(device->CreateTexture2D(&desc, nullptr, &staging), "snapshot staging texture");
            width = input.width; height = input.height; format = input.format;
        }
        lease.submitted = true;
        context->CopyResource(staging.Get(), source.Get()); context->Flush();
        D3D11_MAPPED_SUBRESOURCE map{};
        const auto deadline = GetTickCount64() + 4000;
        for (;;) {
            const auto hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (SUCCEEDED(hr)) break;
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) Check(hr, "snapshot staging Map");
            if (GetTickCount64() >= deadline) throw std::runtime_error("GPU snapshot copy timed out; lease retained");
            Sleep(1);
        }
        lease.completed = true; // Successful Map proves the source GPU read finished.
        struct Unmap {
            ID3D11DeviceContext* context; ID3D11Texture2D* texture;
            ~Unmap() { context->Unmap(texture, 0); }
        } unmap{context.Get(), staging.Get()};
        Frame frame;
        frame.width = input.width; frame.height = input.height; frame.stride = input.width * 4;
        frame.kind = input.kind; frame.sequence = input.sequence; frame.timestamp100ns = input.timestamp100ns;
        frame.bgra.resize(size_t(frame.stride) * frame.height);
        for (uint32_t y = 0; y < frame.height; ++y) {
            auto* target = frame.bgra.data() + size_t(y) * frame.stride;
            memcpy(target, static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, frame.stride);
            for (uint32_t x = 0; x < frame.width; ++x) {
                if (format == DXGI_FORMAT_R8G8B8A8_UNORM) std::swap(target[x * 4], target[x * 4 + 2]);
                target[x * 4 + 3] = 255;
            }
        }
        return frame;
    }
};
void SavePng(const fs::path& path, const Frame& frame) {
    if (fs::exists(path)) throw std::runtime_error("Refusing to overwrite a previous PNG");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "PNG factory");
    ComPtr<IWICStream> stream; Check(factory->CreateStream(&stream), "PNG stream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG file");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialize");
    ComPtr<IWICBitmapFrameEncode> image;
    Check(encoder->CreateNewFrame(&image, nullptr), "PNG image"); Check(image->Initialize(nullptr), "PNG image initialize");
    Check(image->SetSize(frame.width, frame.height), "PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(image->SetPixelFormat(&format), "PNG pixel format");
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed requested BGRA format");
    Check(image->WritePixels(frame.height, frame.stride, UINT(frame.bgra.size()), const_cast<BYTE*>(frame.bgra.data())), "PNG pixels");
    Check(image->Commit(), "PNG image commit"); Check(encoder->Commit(), "PNG commit");
}
uint64_t Hash(const Frame& frame) {
    uint64_t value = 14695981039346656037ull;
    for (uint8_t b : frame.bgra) { value ^= b; value *= 1099511628211ull; }
    return value;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::cout << "GpuCaptureSnapshots --pid PID --output-dir DIR [--frames 3] [--interval-ms 2000]\n"
            "Diagnostic shared-GPU SBS PNGs; no injection or game control.\n"
            "Use a separate short capture session: this consumes frames and competes with a connected sender.\n"
            "GPU read completion is required before lease release; ambiguous failures quarantine that lease.\n";
        return 0;
    }
    bool com{};
    try {
        const auto options = Parse(argc, argv);
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialize"); com = true;
        fs::create_directories(options.output);
        const auto reportPath = options.output / L"gpu-snapshots.jsonl";
        if (fs::exists(reportPath)) throw std::runtime_error("Refusing to overwrite previous snapshot metadata");
        std::ofstream report(reportPath);
        if (!report) throw std::runtime_error("Cannot create snapshot metadata");
        const auto start = GetTickCount64(), deadline = start + 20000;
        const auto freshFloor = Timestamp100ns();
        GpuFrameBus bus; std::string error;
        while (!bus.Open(options.pid, false, error)) {
            if (GetTickCount64() - start >= 3000) throw std::runtime_error(error);
            Sleep(25);
        }
        Reader reader;
        uint64_t after{};
        for (uint32_t n = 1; n <= options.frames; ++n) {
            const auto due = start + uint64_t(n - 1) * options.intervalMs;
            while (GetTickCount64() < due) Sleep(10);
            GpuFrame input;
            for (;;) {
                if (GetTickCount64() >= deadline) throw std::runtime_error("No fresh shared GPU frame before snapshot deadline");
                if (!bus.AcquireLatest(input, after)) { Sleep(10); continue; }
                after = input.sequence;
                if (input.timestamp100ns >= freshFloor) break;
                Lease stale{bus, input.sequence};
                if (!stale.Release()) throw std::runtime_error("Cannot release stale snapshot frame");
            }
            Lease lease{bus, input.sequence};
            const auto readStarted = Timestamp100ns();
            const auto frame = reader.Read(input, lease);
            const auto readCompleted = Timestamp100ns();
            if (!lease.Release()) throw std::runtime_error("Cannot release completed snapshot GPU lease");
            std::ostringstream name; name << "sample-" << std::setw(2) << std::setfill('0') << n << "-sbs.png";
            SavePng(options.output / name.str(), frame);
            std::ostringstream line;
            line << "{\"sample\":" << n << ",\"pid\":" << options.pid << ",\"file\":\"" << name.str()
                << "\",\"sequence\":" << frame.sequence << ",\"timestamp100ns\":" << frame.timestamp100ns
                << ",\"readBegin100ns\":" << readStarted << ",\"readEnd100ns\":" << readCompleted
                << ",\"width\":" << frame.width << ",\"height\":" << frame.height << ",\"kind\":" << uint32_t(frame.kind)
                << ",\"sourceDxgiFormat\":" << uint32_t(input.format) << ",\"adapterLuidLow\":" << input.adapterLuid.LowPart
                << ",\"adapterLuidHigh\":" << input.adapterLuid.HighPart << ",\"leaseReleased\":true,\"alpha\":\"opaque\",\"pixelHashFnv1a64\":\""
                << std::hex << std::setw(16) << std::setfill('0') << Hash(frame) << "\"}";
            report << line.str() << '\n'; report.flush();
            if (!report) throw std::runtime_error("Snapshot metadata write failed");
            std::cout << line.str() << '\n' << std::flush;
        }
        CoUninitialize(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "GpuCaptureSnapshots: " << error.what() << '\n' << std::flush;
        if (com) CoUninitialize();
        return 1;
    }
}
