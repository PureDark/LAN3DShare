#include "capture/DesktopCapture.h"
#include "common/FrameBus.h"
#include <Windows.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using lan3d::Frame;
constexpr ULONGLONG RunLimitMs = 20000, SampleWaitMs = 1200;
constexpr uint64_t MaxDesktopBytes = 512ull * 1024 * 1024;
struct Options { uint32_t pid{}, frames{3}, intervalMs{2000}; fs::path output; bool gdi{true}; };
struct Monitor {
    HMONITOR handle{};
    RECT rect{};
    std::wstring name;
    std::unique_ptr<lan3d::DesktopCapture> capture;
    std::string error;
};
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return "<invalid UTF-16>";
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::string Json(const std::string& value) {
    std::ostringstream result;
    result << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') result << '\\' << c;
        else if (c < 32) result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else result << c;
    }
    result << '"';
    return result.str();
}
uint32_t Number(const wchar_t* text) {
    if (!text || !*text || *text == L'-') throw std::runtime_error("invalid numeric argument");
    wchar_t* end{}; errno = 0;
    const auto result = std::wcstoull(text, &end, 10);
    if (errno || !end || *end || result > UINT32_MAX) throw std::runtime_error("invalid numeric argument");
    return static_cast<uint32_t>(result);
}
Options Parse(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring name = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing argument value");
        const auto value = argv[++i];
        if (name == L"--pid") options.pid = Number(value);
        else if (name == L"--output-dir") options.output = value;
        else if (name == L"--frames") options.frames = Number(value);
        else if (name == L"--interval-ms") options.intervalMs = Number(value);
        else if (name == L"--desktop") {
            if (std::wstring(value) == L"gdi") options.gdi = true;
            else if (std::wstring(value) == L"duplication") options.gdi = false;
            else throw std::runtime_error("--desktop must be gdi or duplication");
        }
        else throw std::runtime_error("unknown option: " + Utf8(name));
    }
    if (!options.pid || options.output.empty() || !options.frames || options.frames > 10 ||
        options.intervalMs < 100 || uint64_t(options.frames - 1) * options.intervalMs + SampleWaitMs > RunLimitMs - 1000)
        throw std::runtime_error("require --pid, --output-dir, frames 1..10 and interval >=100 ms within the 20-second limit");
    options.output = fs::absolute(options.output);
    return options;
}
BOOL CALLBACK AddMonitor(HMONITOR handle, HDC, LPRECT, LPARAM context) {
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(context);
    if (monitors.size() == 16) return FALSE;
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return FALSE;
    Monitor monitor;
    monitor.handle = handle; monitor.rect = info.rcMonitor; monitor.name = info.szDevice;
    monitors.push_back(std::move(monitor));
    return TRUE;
}
std::vector<Monitor> Monitors() {
    std::vector<Monitor> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&monitors)) || monitors.empty())
        throw std::runtime_error("cannot enumerate desktop monitors (maximum 16)");
    return monitors;
}
bool SameLayout(const std::vector<Monitor>& a, const std::vector<Monitor>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].handle != b[i].handle || a[i].name != b[i].name || !EqualRect(&a[i].rect, &b[i].rect)) return false;
    return true;
}
bool CaptureGdi(const RECT& rect, uint64_t sequence, Frame& frame, std::string& error) {
    const auto width = int64_t(rect.right) - rect.left, height = int64_t(rect.bottom) - rect.top;
    if (width <= 0 || height <= 0 || width > LONG_MAX || height > LONG_MAX || uint64_t(width) * height * 4 > MaxDesktopBytes) {
        error = "GDI monitor dimensions exceed the snapshot limit"; return false;
    }
    struct GdiObjects {
        HDC screen{}, memory{};
        HBITMAP bitmap{};
        HGDIOBJ original{};
        ~GdiObjects() {
            if (memory && original) SelectObject(memory, original);
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (screen) ReleaseDC(nullptr, screen);
        }
    } objects;
    const auto failed = [&](const char* operation) {
        error = std::string(operation) + " failed (Win32 " + std::to_string(GetLastError()) + ')'; return false;
    };
    objects.screen = GetDC(nullptr);
    if (!objects.screen) return failed("GetDC desktop");
    objects.memory = CreateCompatibleDC(objects.screen);
    if (!objects.memory) return failed("CreateCompatibleDC");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width); info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    objects.bitmap = CreateDIBSection(objects.screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!objects.bitmap || !pixels) return failed("CreateDIBSection");
    objects.original = SelectObject(objects.memory, objects.bitmap);
    if (!objects.original || objects.original == HGDI_ERROR) { objects.original = nullptr; return failed("SelectObject bitmap"); }
    // Capture the composed screen at the real virtual-desktop coordinates.
    // This is a GDI display snapshot, not the game's raw Present resource.
    if (!BitBlt(objects.memory, 0, 0, static_cast<int>(width), static_cast<int>(height), objects.screen,
        rect.left, rect.top, SRCCOPY | CAPTUREBLT)) return failed("BitBlt desktop");
    if (!GdiFlush()) return failed("GdiFlush");
    frame.width = static_cast<uint32_t>(width); frame.height = static_cast<uint32_t>(height); frame.stride = frame.width * 4;
    frame.sequence = sequence; frame.timestamp100ns = lan3d::Timestamp100ns(); frame.kind = lan3d::ContentKind::Desktop;
    frame.bgra.resize(size_t(frame.stride) * frame.height);
    std::memcpy(frame.bgra.data(), pixels, frame.bgra.size());
    // GDI's 32-bit BI_RGB alpha byte is unspecified. Keep RGB unchanged and
    // normalize only alpha for stable hashes and opaque image viewers.
    for (size_t i = 3; i < frame.bgra.size(); i += 4) frame.bgra[i] = 255;
    error.clear(); return true;
}
uint64_t PixelHash(const Frame& frame) {
    uint64_t result = 14695981039346656037ull;
    for (uint32_t y = 0; y < frame.height; ++y)
        for (size_t x = 0; x < size_t(frame.width) * 4; ++x) {
            result ^= frame.bgra[size_t(y) * frame.stride + x]; result *= 1099511628211ull;
        }
    return result;
}
void SaveBmp(const fs::path& path, const Frame& frame) {
    const uint64_t pixels = uint64_t(frame.width) * frame.height * 4;
    if (!frame.width || !frame.height || frame.width > LONG_MAX || frame.height > LONG_MAX ||
        frame.stride < uint64_t(frame.width) * 4 || uint64_t(frame.stride) * frame.height > frame.bgra.size() ||
        pixels > MaxDesktopBytes) throw std::runtime_error("invalid or oversized BMP pixels");
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(frame.width); info.biHeight = -static_cast<LONG>(frame.height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB; info.biSizeImage = static_cast<DWORD>(pixels);
    // A top-down BGRA bitmap needs no codec or pixel rearrangement.
    if (fs::exists(path)) throw std::runtime_error("refusing to overwrite a previous snapshot: " + Utf8(path.wstring()));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create BMP: " + Utf8(path.wstring()));
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (uint32_t y = 0; y < frame.height; ++y)
        output.write(reinterpret_cast<const char*>(frame.bgra.data() + size_t(y) * frame.stride), std::streamsize(frame.width) * 4);
    output.close();
    if (!output) throw std::runtime_error("BMP write failed: " + Utf8(path.wstring()));
}
void FrameFields(std::ostream& out, const Frame& frame) {
    out << "\"sequence\":" << frame.sequence << ",\"timestamp100ns\":" << frame.timestamp100ns <<
        ",\"width\":" << frame.width << ",\"height\":" << frame.height << ",\"stride\":" << frame.stride <<
        ",\"pixelHashFnv1a64\":\"" << std::hex << std::setw(16) << std::setfill('0') << PixelHash(frame) << std::dec << '"';
}
std::string Prefix(uint32_t sample) {
    std::ostringstream name; name << "sample-" << std::setw(2) << std::setfill('0') << sample; return name.str();
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h")) {
        std::cout << "CaptureSnapshots --pid PID --output-dir PATH [--frames 3] [--interval-ms 2000] [--desktop gdi|duplication]\n"
            "Read-only existing SBS frame bus and full desktop BMP snapshots; no injection, game input or audio.\n"
            "Desktop defaults to GDI composed-screen capture, not a raw Present texture; duplication is an explicit diagnostic mode.\n";
        return 0;
    }
    try {
        const auto options = Parse(argc, argv);
        const auto start = GetTickCount64(), deadline = start + RunLimitMs;
        const auto captureFloor = lan3d::Timestamp100ns();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        fs::create_directories(options.output);
        std::ofstream report(options.output / L"snapshots.jsonl", std::ios::app);
        if (!report) throw std::runtime_error("cannot create snapshots.jsonl");
        auto monitors = Monitors();
        RECT bounds = monitors.front().rect;
        for (const auto& monitor : monitors) {
            bounds.left = std::min(bounds.left, monitor.rect.left); bounds.top = std::min(bounds.top, monitor.rect.top);
            bounds.right = std::max(bounds.right, monitor.rect.right); bounds.bottom = std::max(bounds.bottom, monitor.rect.bottom);
        }
        const auto width = int64_t(bounds.right) - bounds.left, height = int64_t(bounds.bottom) - bounds.top;
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || uint64_t(width) * height * 4 > MaxDesktopBytes)
            throw std::runtime_error("virtual desktop exceeds the bounded 512 MiB snapshot limit");
        for (size_t i = 0; !options.gdi && i < monitors.size() && GetTickCount64() < deadline; ++i) {
            auto& monitor = monitors[i];
            monitor.capture = std::make_unique<lan3d::DesktopCapture>();
            if (!monitor.capture->Open(static_cast<unsigned>(i), monitor.error)) monitor.capture.reset();
        }
        lan3d::FrameBus bus;
        std::string busError;
        bool busOpen = bus.Open(options.pid, false, busError);
        uint64_t lastSbsSequence{};
        // Discard the pre-existing bus image so a frozen previous capture is
        // never mislabeled as a fresh BeforeWeave sample from this run.
        if (busOpen) { Frame baseline; if (bus.ReadLatest(baseline, 0)) lastSbsSequence = baseline.sequence; }
        const auto sampleStart = GetTickCount64();
        uint32_t completed{};
        for (uint32_t sample = 1; sample <= options.frames && GetTickCount64() < deadline; ++sample) {
            const auto scheduled = sampleStart + uint64_t(sample - 1) * options.intervalMs;
            while (GetTickCount64() < std::min(scheduled, deadline)) Sleep(10);
            if (GetTickCount64() >= deadline) break;
            if (!SameLayout(monitors, Monitors())) throw std::runtime_error("desktop layout changed; stopped to avoid incorrect monitor placement");
            const auto begin = lan3d::Timestamp100ns();
            const auto sampleDeadline = std::min(deadline, GetTickCount64() + SampleWaitMs);
            if (!busOpen) {
                busOpen = bus.Open(options.pid, false, busError);
                if (busOpen) { Frame baseline; if (bus.ReadLatest(baseline, 0)) lastSbsSequence = baseline.sequence; }
            }
            for (auto& monitor : monitors) if (monitor.capture && sample > 1) monitor.capture->RequestRefresh();
            std::vector<Frame> desktopFrames(monitors.size());
            std::vector<bool> desktopReady(monitors.size());
            if (options.gdi) {
                for (size_t i = 0; i < monitors.size() && GetTickCount64() < sampleDeadline; ++i)
                    desktopReady[i] = CaptureGdi(monitors[i].rect, sample, desktopFrames[i], monitors[i].error);
            }
            Frame sbs;
            bool newSbs = false;
            while (GetTickCount64() < sampleDeadline) {
                if (busOpen && !newSbs && bus.ReadLatest(sbs, lastSbsSequence)) {
                    lastSbsSequence = sbs.sequence;
                    newSbs = sbs.kind == lan3d::ContentKind::LeiaSbs && sbs.timestamp100ns >= captureFloor;
                    if (!newSbs) busError = sbs.kind != lan3d::ContentKind::LeiaSbs ?
                        "frame bus published ordinary desktop content, not LeiaSbs" : "frame bus image predates this diagnostic run";
                }
                bool allDesktop = true;
                for (size_t i = 0; i < monitors.size(); ++i) {
                    auto& monitor = monitors[i];
                    if (!monitor.capture || desktopReady[i]) continue;
                    const auto result = monitor.capture->Capture(desktopFrames[i], monitor.error);
                    if (result == lan3d::DesktopCaptureResult::FrameReady) {
                        const auto& frame = desktopFrames[i];
                        desktopReady[i] = frame.width == uint32_t(monitor.rect.right - monitor.rect.left) &&
                            frame.height == uint32_t(monitor.rect.bottom - monitor.rect.top);
                        if (!desktopReady[i]) monitor.error = "duplication dimensions do not match monitor coordinates";
                    }
                    allDesktop = allDesktop && desktopReady[i];
                }
                if (allDesktop && newSbs) break;
                Sleep(5);
            }
            const auto prefix = Prefix(sample);
            Frame desktop;
            desktop.width = static_cast<uint32_t>(width); desktop.height = static_cast<uint32_t>(height); desktop.stride = desktop.width * 4;
            desktop.sequence = sample; desktop.timestamp100ns = lan3d::Timestamp100ns(); desktop.kind = lan3d::ContentKind::Desktop;
            desktop.bgra.resize(size_t(desktop.stride) * desktop.height);
            size_t captured{};
            for (size_t i = 0; i < monitors.size(); ++i) if (desktopReady[i]) {
                ++captured;
                const auto& source = desktopFrames[i];
                const auto x = static_cast<size_t>(int64_t(monitors[i].rect.left) - bounds.left);
                const auto y = static_cast<size_t>(int64_t(monitors[i].rect.top) - bounds.top);
                for (uint32_t row = 0; row < source.height; ++row)
                    std::memcpy(desktop.bgra.data() + (y + row) * desktop.stride + x * 4,
                        source.bgra.data() + size_t(row) * source.stride, size_t(source.width) * 4);
            }
            const auto desktopFile = prefix + (captured == monitors.size() ? "-desktop.bmp" : "-desktop-partial.bmp");
            if (captured) SaveBmp(options.output / desktopFile, desktop);
            const auto sbsFile = prefix + "-sbs.bmp";
            if (newSbs) SaveBmp(options.output / sbsFile, sbs);
            std::ostringstream entry;
            entry << "{\"sample\":" << sample << ",\"pid\":" << options.pid << ",\"beginTimestamp100ns\":" << begin <<
                ",\"endTimestamp100ns\":" << lan3d::Timestamp100ns() <<
                ",\"clock\":\"QPC-derived 100ns, shared local clock\",\"note\":\"SBS and composed desktop are independent samples; desktop includes all monitors, no crop or pointer composition. Desktop is not a raw Present texture\",\"sbs\":{";
            if (newSbs) { entry << "\"status\":\"new_frame\",\"file\":" << Json(sbsFile) << ','; FrameFields(entry, sbs); }
            else entry << "\"status\":\"no_new_frame\",\"afterSequence\":" << lastSbsSequence << ",\"error\":" << Json(busError);
            entry << ",\"captureStatus\":" << Json(busOpen ? bus.Status() : "unavailable") << "},\"desktop\":{\"captureApi\":" <<
                Json(options.gdi ? "GDI" : "DXGI Desktop Duplication") << ",\"alpha\":" <<
                Json(options.gdi ? "normalized to opaque; RGB unchanged" : "as captured") << ",\"status\":" <<
                Json(captured == monitors.size() ? "complete" : captured ? "partial_missing_monitors_are_black" : "unavailable") <<
                ",\"left\":" << bounds.left << ",\"top\":" << bounds.top << ",\"monitors\":[";
            for (size_t i = 0; i < monitors.size(); ++i) {
                if (i) entry << ',';
                const auto& monitor = monitors[i];
                entry << "{\"index\":" << i << ",\"name\":" << Json(Utf8(monitor.name)) << ",\"left\":" << monitor.rect.left <<
                    ",\"top\":" << monitor.rect.top << ",\"status\":" << Json(desktopReady[i] ? "captured" : "unavailable");
                if (desktopReady[i]) { entry << ','; FrameFields(entry, desktopFrames[i]); }
                else entry << ",\"error\":" << Json(monitor.error);
                entry << '}';
            }
            entry << ']';
            if (captured) { entry << ",\"file\":" << Json(desktopFile) << ','; FrameFields(entry, desktop); }
            entry << "}}";
            report << entry.str() << '\n'; report.flush();
            if (!report) throw std::runtime_error("snapshot metadata write failed");
            std::cout << entry.str() << std::endl;
            ++completed;
        }
        if (completed != options.frames) { std::cerr << "Stopped at the 20-second run deadline after " << completed << " samples.\n"; return 2; }
        return 0;
    } catch (const std::exception& error) { std::cerr << "CaptureSnapshots: " << error.what() << '\n'; return 1; }
}
