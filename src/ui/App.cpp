#include "update/Updater.h"
#include "Version.h"
#include "media/GpuAdapter.h"
#include "AudioMeterModel.h"
#include "App.h"
#include "WorkerProcess.h"
#include "WebViewHost.h"
#include "ViewerControl.h"
#include "PreviewChannel.h"
#include "app/Injector.h"
#include "session/PeerService.h"
#include "nlohmann/json.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <string>
#include <vector>

namespace lan3d::ui {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n) return {};
    std::wstring out(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n); return out;
}
std::string Utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr); return out;
}
std::string Guid(bool token = false) {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create session identity");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    auto value = Utf8(text); value.erase(std::remove(value.begin(), value.end(), '{'), value.end()); value.erase(std::remove(value.begin(), value.end(), '}'), value.end());
    if (token) value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(tolower(c)); }); return value;
}
std::filesystem::path DataDirectory() {
    PWSTR path{}; if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) throw std::runtime_error("Local application data is unavailable");
    auto result = std::filesystem::path(path) / L"LAN3DShare"; CoTaskMemFree(path); std::filesystem::create_directories(result); return result;
}
std::wstring ChoosePath(HWND owner, bool directory) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags{}; dialog->GetOptions(&flags); dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (directory ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dialog->SetTitle(directory ? L"选择 LeiaSR 或 LeiaSRBridge 的完整 runtime 目录" : L"选择要共享的 3D 程序");
    COMDLG_FILTERSPEC filter{L"Windows 程序", L"*.exe"}; if (!directory) dialog->SetFileTypes(1, &filter);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item; PWSTR path{};
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring result(path); CoTaskMemFree(path); return result;
}
bool RuntimeFound(const std::wstring& path) {
    std::error_code e; return std::filesystem::is_regular_file(std::filesystem::path(path) / L"SimulatedRealityDirectX.dll", e) &&
        std::filesystem::is_regular_file(std::filesystem::path(path) / L"SimulatedRealityCore.dll", e);
}
struct Display { HMONITOR handle{}; std::wstring name; int width{}, height{}; RECT bounds{}; };
std::vector<Display> Displays() {
    std::vector<Display> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM value) -> BOOL {
        auto& list = *reinterpret_cast<std::vector<Display>*>(value); MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfoW(m, &info)) {
            const int w = info.rcMonitor.right - info.rcMonitor.left, h = info.rcMonitor.bottom - info.rcMonitor.top;
            list.push_back({m, L"屏幕 " + std::to_wstring(list.size() + 1), w, h, info.rcMonitor});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result)); return result;
}
std::string ImageDataUri(Gdiplus::Image& image) {
    std::string output;
    {
        ComPtr<IStream> stream;
        const CLSID jpeg{0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && image.Save(stream.Get(), &jpeg) == Gdiplus::Ok) {
            STATSTG stat{}; HGLOBAL storage{};
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream.Get(), &storage))) {
                auto bytes = static_cast<const unsigned char*>(GlobalLock(storage));
                if (bytes) {
                    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    output = "data:image/jpeg;base64,"; const auto size = size_t(stat.cbSize.QuadPart);
                    for (size_t i = 0; i < size; i += 3) {
                        const unsigned n = (unsigned(bytes[i]) << 16) | (i + 1 < size ? unsigned(bytes[i + 1]) << 8 : 0) | (i + 2 < size ? bytes[i + 2] : 0);
                        output += alphabet[n >> 18]; output += alphabet[(n >> 12) & 63];
                        output += i + 1 < size ? alphabet[(n >> 6) & 63] : '='; output += i + 2 < size ? alphabet[n & 63] : '=';
                    }
                    GlobalUnlock(storage);
                }
            }
        }
    }
    return output;
}
// Small previews stay in memory, independently of the encoded video stream.
std::string Preview(const Display& d) {
    if (d.width <= 0 || d.height <= 0) return {};
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    const int width = 432, height = std::clamp(width * d.height / d.width, 1, 432);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height); auto old = SelectObject(memory, bitmap);
    SetStretchBltMode(memory, HALFTONE);
    const bool copied = StretchBlt(memory, 0, 0, width, height, screen, d.bounds.left, d.bounds.top, d.width, d.height, SRCCOPY) != FALSE;
    SelectObject(memory, old); DeleteDC(memory); ReleaseDC(nullptr, screen);
    std::string output;
    if (copied) { Gdiplus::Bitmap image(bitmap, nullptr); output = ImageDataUri(image); }
    DeleteObject(bitmap); return output;
}
std::string Preview(const Frame& f) {
    if (!ValidFrame(f)) return {};
    Gdiplus::Bitmap image(int(f.width), int(f.height), int(f.stride), PixelFormat32bppARGB, const_cast<BYTE*>(f.bgra.data()));
    return ImageDataUri(image);
}
struct Settings {
    std::string id, resolution{"4k"}, gpu{"auto"};
    std::wstring name, runtime{L"C:\\Program Files\\LeiaSR\\Platform\\bin"};
    int monitor{}, fps{60}, rate{200}, receiverVolumePercent{100}; std::filesystem::path file;
    void Load(const std::filesystem::path& directory) {
        file = directory / L"settings.ini";
        wchar_t computer[256]{}; DWORD count = 256; GetComputerNameW(computer, &count); wchar_t buffer[32768]{};
        GetPrivateProfileStringW(L"Client", L"Name", computer, buffer, DWORD(std::size(buffer)), file.c_str()); name = buffer;
        GetPrivateProfileStringW(L"Client", L"Id", L"", buffer, DWORD(std::size(buffer)), file.c_str()); id = Utf8(buffer); if (id.size() != 36) id = Guid();
        GetPrivateProfileStringW(L"Client", L"Runtime", runtime.c_str(), buffer, DWORD(std::size(buffer)), file.c_str()); runtime = buffer;
        GetPrivateProfileStringW(L"Client", L"EncodeGpu", L"auto", buffer, DWORD(std::size(buffer)), file.c_str()); gpu = Utf8(buffer);
        monitor = int(GetPrivateProfileIntW(L"Client", L"Monitor", 0, file.c_str()));
        const int old = int(GetPrivateProfileIntW(L"Client", L"Quality", 0, file.c_str()));
        resolution = old == 1 ? "1080p" : "4k"; fps = old == 1 ? 30 : 60; rate = old == 1 ? 20 : old == 2 ? 100 : old == 3 ? 400 : 200;
        GetPrivateProfileStringW(L"Client", L"Resolution", Wide(resolution).c_str(), buffer, DWORD(std::size(buffer)), file.c_str()); resolution = Utf8(buffer);
        if (resolution != "720p" && resolution != "1080p" && resolution != "2k" && resolution != "4k") resolution = "4k";
        fps = int(GetPrivateProfileIntW(L"Client", L"FPS", fps, file.c_str())); if (fps != 30 && fps != 60 && fps != 90 && fps != 120) fps = 60;
        rate = int(GetPrivateProfileIntW(L"Client", L"BitrateMbps", rate, file.c_str()));
        rate = rate == 0 ? 0 : std::clamp(rate, 20, 1000);
        receiverVolumePercent = std::clamp(int(GetPrivateProfileIntW(L"Client", L"ReceiverVolumePercent", 100, file.c_str())), 0, 800);
        WritePrivateProfileStringW(L"Client", L"AudioGainPercent", nullptr, file.c_str());
        Save();
    }
    bool Save() const {
        if (file.empty()) return true;
        bool ok = true;
        auto write = [&](const wchar_t* key, const std::wstring& value) { if (!WritePrivateProfileStringW(L"Client", key, value.c_str(), file.c_str())) ok = false; };
        write(L"EncodeGpu", Wide(gpu)); write(L"Name", name); write(L"Id", Wide(id)); write(L"Runtime", runtime);
        write(L"Monitor", std::to_wstring(monitor)); write(L"Resolution", Wide(resolution));
        write(L"FPS", std::to_wstring(fps)); write(L"BitrateMbps", std::to_wstring(rate));
        write(L"ReceiverVolumePercent", std::to_wstring(receiverVolumePercent));
        return ok;
    }
};
struct Source { enum Kind { Screen, Process, Program, Pattern } kind{Screen}; std::string id; std::wstring name, path; uint32_t pid{}, monitor{}; };
enum class Phase { Idle, PreparingSend, Requesting, Sending, PreparingReceive, Receiving, Stopping };
struct App {
    HINSTANCE instance{}; HWND window{}; WebViewHost web;
    std::filesystem::path executable, dataDir, smokePath; std::string smokeState{"idle"};
    std::wstring testRole; uint16_t testPort{}, testTarget{};
    std::unique_ptr<update::Client> updater; bool updateReserved{};
    Settings settings; std::vector<Display> displays; std::vector<Source> sources; std::vector<session::Peer> peers;
    std::string selectedSource, selectedPeer, contentKind{"desktop"}, preview, receivedPreview;
    session::Service service; session::Peer current; std::string token; uint64_t incoming{};
    WorkerProcess worker{WorkerProcessOptions{35000, 1500}};
    Phase phase{Phase::Idle}; Clock::time_point phaseTime{}, testStart{Clock::now()};
    bool volumeSavePending{}; Clock::time_point volumeChangedAt{};
    std::future<std::pair<bool, std::string>> request;
    unsigned ticks{}, readyTicks{}; bool closing{}, smoke{}, smokeCaptured{}, testStarted{}, testConnected{}, testSucceeded{}, testDesktop{}, networkReady{}, uiReady{}, uiFailed{}, pendingSettings{}, videoRecovering{};
    std::wstring note, encodingSummary, sourceNotice;
    int Scale(int px) const { return MulDiv(px, window ? int(GetDpiForWindow(window)) : 96, 96); }
    void SetNote(std::wstring text) { note = std::move(text); }
    bool StartService() {
        if (smoke) return networkReady = true;
        session::Config c; c.localId = settings.id; c.displayName = Utf8(settings.name);
        if (!testRole.empty()) { c.bindAddress = "127.0.0.1"; c.enableBroadcast = false; c.discoveryPort = testPort; c.testTargets.push_back({"127.0.0.1", testTarget}); }
        std::string error; networkReady = service.Start(c, error);
        if (!networkReady) SetNote(L"无法启动局域网发现：" + Wide(error)); return networkReady;
    }
    void RefreshSources() {
        if (phase != Phase::Idle) return;
        auto old = sources; sources.clear(); displays = Displays();
        for (size_t i = 0; i < displays.size(); ++i) sources.push_back({Source::Screen, "screen:" + std::to_string(i), displays[i].name, {}, 0, unsigned(i)});
        if (!smoke && testRole.empty()) for (const auto& p : ListProcesses()) if (p.leiaLoaded && _wcsicmp(p.name.c_str(), L"LAN3DShare.exe")) sources.push_back({Source::Process, "pid:" + std::to_string(p.id), p.name, {}, p.id, 0});
        for (const auto& item : old) if (item.kind == Source::Program) sources.push_back(item);
        sources.push_back({Source::Pattern, "pattern", L"3D 测试画面"});
        if (std::none_of(sources.begin(), sources.end(), [&](const Source& s) { return s.id == selectedSource; })) selectedSource = sources.front().id;
    }
    void RefreshPeers() {
        peers = service.Snapshot();
        if (std::none_of(peers.begin(), peers.end(), [&](const session::Peer& p) { return p.id == selectedPeer; })) {
            selectedPeer.clear(); for (const auto& p : peers) if (p.state == session::State::Idle) { selectedPeer = p.id; break; }
        }
    }
    Json Model() const {
        const char* phases[]{"idle", "preparingSend", "requesting", "sending", "preparingReceive", "receiving", "stopping"};
        Json m{{"type", "state"}, {"phase", phases[int(phase)]}, {"localName", Utf8(settings.name)}, {"networkReady", networkReady},
            {"note", Utf8(note)},
            {"sourceNotice", Utf8(phase == Phase::Sending ? sourceNotice : L"")},
            {"selectedSource", selectedSource}, {"selectedPeer", selectedPeer}, {"quality", {{"res", settings.resolution}, {"fps", settings.fps}, {"rate", settings.rate}}},
            {"settings", {{"name", Utf8(settings.name)}, {"monitor", settings.monitor}, {"gpu", settings.gpu}, {"runtime", Utf8(settings.runtime)}, {"runtimeFound", RuntimeFound(settings.runtime)}}},
            {"volumePercent", settings.receiverVolumePercent},
            {"peer", {{"name", current.name}, {"address", current.address}}}, {"busy", phase != Phase::Idle || closing}, {"contentKind", contentKind},
            {"elapsedSeconds", phase == Phase::Sending || phase == Phase::Receiving ? std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - phaseTime).count() : 0},
            {"stats", videoRecovering ? (phase == Phase::Sending && settings.rate != 0 ? "正在恢复视频连接；码率过高时请降低或选择自动" : "网络暂时拥塞，正在恢复画面…") : Utf8(encodingSummary)},
            {"maximized", IsZoomed(window) != FALSE}, {"sources", Json::array()}, {"peers", Json::array()}, {"monitors", Json::array()}};
        m["update"] = updater ? updater->Model() : Json{{"current", LAN3D_VERSION}, {"status", "idle"}, {"message", "检查 GitHub 最新版本"}, {"busy", false}};
        m["gpus"] = Json::array();
        for (const auto& gpu : media::EncodeAdapters()) m["gpus"].push_back({{"id", gpu.id}, {"name", "GPU " + std::to_string(gpu.index) + " · " + Utf8(gpu.name) + (gpu.id.starts_with("pci:") ? "（PCI " + gpu.id.substr(4) + "）" : "（PCI 地址不可用）")}});
        for (size_t i = 0; i < displays.size(); ++i) m["monitors"].push_back({{"id", i}, {"name", Utf8(displays[i].name)}, {"width", displays[i].width}, {"height", displays[i].height}});
        for (const auto& s : sources) {
            const int w = s.kind == Source::Screen && s.monitor < displays.size() ? displays[s.monitor].width : 0;
            const int h = s.kind == Source::Screen && s.monitor < displays.size() ? displays[s.monitor].height : 0;
            const char* kinds[]{"screen", "process", "program", "test"};
            const auto sub = w ? std::to_string(w) + " × " + std::to_string(h) : s.kind == Source::Process ? "LeiaSR 程序" : s.kind == Source::Program ? "连接时启动" : "无需启动游戏";
            m["sources"].push_back({{"id", s.id}, {"name", Utf8(s.name)}, {"sub", sub}, {"kind", kinds[int(s.kind)]}, {"width", w}, {"height", h}});
        }
        for (const auto& p : peers) {
            const char* state = p.state == session::State::Idle ? "idle" : p.state == session::State::Sending ? "sending" : p.state == session::State::Receiving ? "receiving" : "busy";
            m["peers"].push_back({{"id", p.id}, {"name", p.name}, {"address", p.address}, {"state", state}});
        }
        audio::Telemetry audioData;
        if ((phase == Phase::Sending || phase == Phase::Receiving) && audio::ReadTelemetry(worker.ProcessId(), audioData))
            m["audio"] = AudioMeterModel(audioData);
        else m["audio"] = nullptr;
        m["preview"] = preview; m["receivedPreview"] = receivedPreview;
        if (smoke) {
            m["designScene"] = true; m["uiState"] = smokeState;
            m["localName"] = "PureDark-Gaming"; m["settings"]["name"] = "PureDark-Gaming";
            m["peers"] = Json::array({{{"id", "a"}, {"name", "Huangwei-Gaming"}, {"address", "192.168.3.197"}, {"state", "idle"}},
                {{"id", "b"}, {"name", "Studio-PC"}, {"address", "192.168.3.108"}, {"state", "idle"}},
                {{"id", "c"}, {"name", "LivingRoom"}, {"address", "192.168.3.63"}, {"state", "receiving"}}});
            m["selectedPeer"] = "a";
        }
        return m;
    }
    void Publish() { if (uiReady) web.Post(Model().dump()); }
    void SaveVolume() {
        if (!volumeSavePending) return;
        volumeSavePending = false;
        if (!settings.file.empty() && !WritePrivateProfileStringW(L"Client", L"ReceiverVolumePercent",
            std::to_wstring(settings.receiverVolumePercent).c_str(), settings.file.c_str()))
            SetNote(L"播放音量已生效，但无法保存，下次连接可能恢复原音量。");
    }
    void SetReceiverVolume(int percent, bool notifyViewer) {
        if (percent < 0 || percent > 800) return;
        settings.receiverVolumePercent = percent; volumeSavePending = true; volumeChangedAt = Clock::now();
        if (notifyViewer && phase == Phase::Receiving) {
            EnumWindows([](HWND h, LPARAM value) -> BOOL {
                auto self = reinterpret_cast<App*>(value); DWORD pid{}; GetWindowThreadProcessId(h, &pid);
                wchar_t cls[128]{}; GetClassNameW(h, cls, 128);
                if (pid == self->worker.ProcessId() && !wcscmp(cls, ViewerWindowClass)) {
                    PostMessageW(h, ViewerVolumeMessage(), WPARAM(self->window), LPARAM(self->settings.receiverVolumePercent)); return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
        }
    }
    void OpenLogs() {
        if (!worker.LogPath().empty()) ShellExecuteW(window, L"open", L"notepad.exe", (L"\"" + worker.LogPath().wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else { std::filesystem::create_directories(dataDir / L"logs"); ShellExecuteW(window, L"open", (dataDir / L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    }
    void ShowReceiver() {
        if (phase != Phase::Receiving) return;
        EnumWindows([](HWND h, LPARAM value) -> BOOL {
            auto self = reinterpret_cast<App*>(value); DWORD pid{}; GetWindowThreadProcessId(h, &pid);
            wchar_t cls[128]{}; GetClassNameW(h, cls, 128);
            if (pid == self->worker.ProcessId() && !wcscmp(cls, ViewerWindowClass)) {
                ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); PostMessageW(h, ViewerFullscreenMessage(), WPARAM(self->window), 0); return FALSE;
            } return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }
    void OnMessage(const std::string& text) {
        if (text.size() > 16384) { web.Post(Json{{"type", "settingsSaved"}, {"ok", false}, {"error", "设置内容过长，请缩短运行时目录。"}}.dump()); return; }
        try {
            const auto j = Json::parse(text); const auto action = j.value("action", std::string{});
            if (action == "ready") { uiReady = true; Publish(); return; }
            if (action.starts_with("update") && updater) {
                if (phase != Phase::Idle || closing || worker.Running() || updater->Busy()) return;
                if (action == "updateCheck") updater->Check();
                else if (action == "updateDownload") updater->Download();
                else if (action == "updateInstall") updater->Install();
                updateReserved = updater->Busy();
                if (updateReserved) service.SetState(session::State::Busy);
                Publish(); return;
            }
            if (action == "saveSettings" && (phase != Phase::Idle || closing)) {
                web.Post(Json{{"type", "settingsSaved"}, {"ok", false}, {"error", "正在共享画面，请断开后再保存设置。"}}.dump()); return;
            }
            if (action == "minimize") ShowWindow(window, SW_MINIMIZE);
            else if (action == "maximize") ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            else if (action == "close") PostMessageW(window, WM_CLOSE, 0, 0);
            else if (action == "drag") { ReleaseCapture(); PostMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, GetMessagePos()); }
            else if (action == "resize" && !IsZoomed(window)) {
                const std::string edge = j.value("edge", std::string{});
                const std::pair<const char*, WPARAM> edges[]{{"left", WMSZ_LEFT}, {"right", WMSZ_RIGHT}, {"top", WMSZ_TOP}, {"bottom", WMSZ_BOTTOM}, {"top-left", WMSZ_TOPLEFT}, {"top-right", WMSZ_TOPRIGHT}, {"bottom-left", WMSZ_BOTTOMLEFT}, {"bottom-right", WMSZ_BOTTOMRIGHT}};
                for (const auto& e : edges) if (edge == e.first) { ReleaseCapture(); PostMessageW(window, WM_SYSCOMMAND, SC_SIZE + e.second, 0); break; }
            }
            else if (action == "logs") OpenLogs();
            else if (action == "fullscreen") ShowReceiver();
            else if (action == "disconnect") StopSession(L"正在结束画面共享。");
            else if (action == "volume" && phase == Phase::Receiving && !closing) {
                if (j.contains("percent") && j["percent"].is_number_integer() && j["percent"] >= 0 && j["percent"] <= 800)
                    SetReceiverVolume(j["percent"].get<int>(), true);
            }
            else if (phase == Phase::Idle && !closing) {
                if (action == "connect") BeginSend();
                else if (action == "selectPeer") { const auto id = j.value("id", std::string{}); for (const auto& p : peers) if (p.id == id && p.state == session::State::Idle) selectedPeer = id; }
                else if (action == "selectSource") { const auto id = j.value("id", std::string{}); for (const auto& s : sources) if (s.id == id) { selectedSource = id; preview.clear(); } }
                else if (action == "refreshSources") RefreshSources();
                else if (action == "launch") {
                    auto path = ChoosePath(window, false); if (!path.empty()) { selectedSource = "program:" + Guid(); sources.push_back({Source::Program, selectedSource, std::filesystem::path(path).filename().wstring(), path}); preview.clear(); }
                } else if (action == "quality") {
                    auto res = j.value("res", settings.resolution); int fps = j.value("fps", settings.fps), rate = j.value("rate", settings.rate);
                    if ((res == "720p" || res == "1080p" || res == "2k" || res == "4k") && (fps == 30 || fps == 60 || fps == 90 || fps == 120) && rate >= 0 && rate <= 1000) {
                        settings.resolution = res; settings.fps = fps; settings.rate = rate == 0 ? 0 : std::max(20, rate);
                        if (!settings.Save()) SetNote(L"画质已应用，但无法保存到设置文件。");
                    }
                } else if (action == "browseRuntime") {
                    const auto path = ChoosePath(window, true); if (!path.empty()) web.Post(Json{{"type", "runtimeSelected"}, {"path", Utf8(path)}, {"found", RuntimeFound(path)}}.dump());
                } else if (action == "saveSettings") {
                    const auto name = j.value("name", std::string{}); const auto runtime = Wide(j.value("runtime", std::string{})); const int monitor = j.value("monitor", 0);
                    const auto gpu = j.value("gpu", settings.gpu);
                    const auto adapters = media::EncodeAdapters();
                    if (gpu != "auto" && std::none_of(adapters.begin(), adapters.end(), [&](const auto& a) { return a.id == gpu; })) {
                        web.Post(Json{{"type", "settingsSaved"}, {"ok", false}, {"error", "所选 GPU 已不可用，请重新选择。"}}.dump()); return;
                    }
                    if (name.empty() || name.size() > 96 || std::all_of(name.begin(), name.end(), [](unsigned char c) { return c == ' '; }) || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }) || runtime.size() > 32760 || monitor < 0 || size_t(monitor) >= displays.size()) {
                        web.Post(Json{{"type", "settingsSaved"}, {"ok", false}, {"error", "请填写有效的设备名称（最多 96 个 UTF-8 字节）和显示器。"}}.dump());
                    }
                    else {
                        const auto previous = settings; const bool renamed = settings.name != Wide(name);
                        settings.name = Wide(name); settings.runtime = runtime; settings.monitor = monitor; settings.gpu = gpu;
                        if (!settings.Save()) {
                            settings = previous; settings.Save();
                            web.Post(Json{{"type", "settingsSaved"}, {"ok", false}, {"error", "无法写入设置文件，请检查目录权限后重试。"}}.dump());
                        } else {
                            if (renamed) { service.Stop(); StartService(); RefreshPeers(); }
                            if (networkReady) SetNote(L"设置已保存。");
                            web.Post(Json{{"type", "settingsSaved"}, {"ok", true}}.dump());
                        }
                    }
                }
            }
            Publish();
        } catch (const Json::exception&) { SetNote(L"界面请求无效，请重试。"); }
    }
    void UpdateEncodingNote() {
        const auto log = worker.TailLog();
        if (phase == Phase::Sending) {
            static const std::wregex recoveryPattern(LR"((?:^|[\r\n])STREAM_(BACKPRESSURE|RECOVERY|READY) )");
            for (auto i = std::wsregex_iterator(log.begin(), log.end(), recoveryPattern); i != std::wsregex_iterator(); ++i) videoRecovering = (*i)[1].str() != L"READY";
            static const std::wregex capturePattern(LR"((?:^|[\r\n])SOURCE_STATE[ \t]+([a-z-]+)(?=[ \t\r\n]|$))");
            for (auto i = std::wsregex_iterator(log.begin(), log.end(), capturePattern); i != std::wsregex_iterator(); ++i) {
                const auto state = (*i)[1].str();
                sourceNotice = state == L"waiting-process" ? L"源程序正在切换进程，等待接入实际游戏…" :
                    state == L"waiting-runtime" ? L"正在等待源程序完成初始化并启用 LeiaSR…" :
                    state == L"waiting-input" ? L"正在等待 3D 原始画面；请保持源程序开启 LeiaSR 并持续渲染。" :
                    state == L"capture-unavailable" ? L"3D 采集未能接入，正在分享普通画面；详情见日志。" :
                    state == L"stale-sbs" ? L"源程序已停止更新 3D 画面，暂时分享普通画面。" :
                    state == L"foreground-desktop" ? L"当前前台是普通画面；固定分享 3D 程序可在画面来源中选择该程序。" : L"";
            }
        }
        static const std::wregex encoderPattern(LR"((?:^|[\r\n])ENCODER (NVENC|Software) (AV1|HEVC|H\.264) (SBS|Desktop) ([0-9]+)x([0-9]+) target=([0-9]+)fps/([0-9]+)Mbps)");
        for (auto i = std::wsregex_iterator(log.begin(), log.end(), encoderPattern); i != std::wsregex_iterator(); ++i) {
            const auto& s = *i;
            encodingSummary = (s[1].str() == L"NVENC" ? L"NVENC " : L"软件回退 · ") + s[2].str() + L" · " + s[4].str() + L"×" + s[5].str() + L" · 目标 " + s[6].str() + L" FPS / " + s[7].str() + L" Mbps";
            contentKind = s[3].str() == L"SBS" ? "sbs" : "desktop";
        }
        static const std::wregex sourcePattern(LR"((?:^|[\r\n])SOURCE_KIND (LeiaSbs|Desktop))");
        for (auto i = std::wsregex_iterator(log.begin(), log.end(), sourcePattern); i != std::wsregex_iterator(); ++i) contentKind = (*i)[1].str() == L"LeiaSbs" ? "sbs" : "desktop";
        if (settings.rate == 0 && phase == Phase::Sending) {
            static const std::wregex ratePattern(LR"((?:^|[\r\n])AUTO_BITRATE target=([0-9]+))");
            for (auto i = std::wsregex_iterator(log.begin(), log.end(), ratePattern); i != std::wsregex_iterator(); ++i) {
                auto separator = encodingSummary.find(L" / "); if (separator != std::wstring::npos) encodingSummary.resize(separator);
                encodingSummary += L" / 自动 " + std::to_wstring(std::stoull((*i)[1].str()) / 1000000) + L" Mbps";
            }
        }
        static const std::wregex receiverPattern(LR"((?:^|[\r\n])VIEWER_STATE (\{[^\r\n]+\}))");
        for (auto i = std::wsregex_iterator(log.begin(), log.end(), receiverPattern); i != std::wsregex_iterator(); ++i) {
            const auto state = Json::parse(Utf8((*i)[1].str()), nullptr, false); if (state.is_discarded()) continue;
            contentKind = state.value("kind", std::string("desktop"));
            encodingSummary = std::to_wstring(state.value("width", 0)) + L"×" + std::to_wstring(state.value("height", 0)) + L" · " + std::to_wstring(state.value("receivingFps", 0)) + L" FPS";
            videoRecovering = state.value("recovering", false);
        }
    }
    void StopSession(std::wstring message) {
        if (phase == Phase::Idle || phase == Phase::Stopping) return;
        if (!token.empty()) service.EndSession(current, token);
        worker.RequestStop(); phase = Phase::Stopping; phaseTime = Clock::now(); SetNote(std::move(message)); Publish();
    }
    void BeginSend() {
        if (phase != Phase::Idle || smoke || !networkReady || updateReserved) return;
        RefreshPeers();
        auto p = std::find_if(peers.begin(), peers.end(), [&](const session::Peer& item) { return item.id == selectedPeer && item.state == session::State::Idle; });
        auto s = std::find_if(sources.begin(), sources.end(), [&](const Source& item) { return item.id == selectedSource; });
        if (p == peers.end() || s == sources.end()) return;
        current = *p; token = Guid(true); incoming = 0; encodingSummary.clear(); sourceNotice.clear(); contentKind = "desktop"; preview.clear(); receivedPreview.clear(); videoRecovering = false;
        const auto& input = *s;
        std::vector<std::wstring> args{L"--port", L"0", L"--session-token", Wide(token)};
        args.insert(args.end(), {L"--gpu", Wide(settings.gpu), L"--profile"});
        if (!testRole.empty()) args.insert(args.end(), {L"--width", L"640", L"--height", L"360", L"--fps", L"10", L"--bitrate", L"12000000", L"--codec", L"h264"});
        else {
            const int width = settings.resolution == "720p" ? 1280 : settings.resolution == "1080p" ? 1920 : settings.resolution == "2k" ? 2560 : 3840;
            args.insert(args.end(), {L"--width", std::to_wstring(width), L"--height", std::to_wstring(width * 9 / 16), L"--fps", std::to_wstring(settings.fps), L"--bitrate", std::to_wstring(settings.rate * 1000000), L"--codec", L"auto"});
        }
        if (input.kind == Source::Pattern) {
            args.insert(args.end(), {L"--test-pattern", L"--source-width", L"1280", L"--source-height", L"720"});
            if (testRole.empty()) args.push_back(L"--audio");
        }
        else {
            args.insert(args.end(), {L"--desktop-monitor", std::to_wstring(input.monitor)});
            if (!testDesktop) args.push_back(L"--auto-source");
            if (input.kind == Source::Process) args.insert(args.end(), {L"--pid", std::to_wstring(input.pid)});
            else if (input.kind == Source::Program) args.insert(args.end(), {L"--launch", input.path});
        }
        service.SetState(session::State::Busy); std::wstring error;
        if (!worker.Start(executable, WorkerMode::Sender, args, error, dataDir / L"logs")) { service.SetState(session::State::Idle); SetNote(L"无法开始分享：" + error); current = {}; token.clear(); return; }
        phase = Phase::PreparingSend; phaseTime = Clock::now(); SetNote(L"正在准备画面，对方将自动打开接收窗口。"); Publish();
    }
    void BeginReceive(const session::Event& event) {
        std::string ignored;
        if (phase != Phase::Idle || closing || updateReserved) { service.Reply(event.requestId, false, "Client is busy", ignored); return; }
        current = event.peer; token = event.token; incoming = event.requestId; contentKind = "desktop"; encodingSummary.clear(); preview.clear(); receivedPreview.clear(); videoRecovering = false;
        displays = Displays(); const int monitor = displays.empty() ? 0 : std::clamp(settings.monitor, 0, int(displays.size()) - 1);
        std::vector<std::wstring> args{L"--profile", L"--connect", Wide(event.peer.address), L"--port", std::to_wstring(event.mediaPort), L"--session-token", Wide(token),
            L"--volume-percent", std::to_wstring(settings.receiverVolumePercent),
            L"--auto-display", L"--monitor", std::to_wstring(monitor), L"--runtime-dir", settings.runtime, L"--peer-name", Wide(current.name), L"--parent-window", std::to_wstring(reinterpret_cast<uintptr_t>(window))};
        if (!testRole.empty()) args.push_back(L"--hidden");
        std::wstring error;
        if (!worker.Start(executable, WorkerMode::Receiver, args, error, dataDir / L"logs")) { service.Reply(incoming, false, Utf8(error), ignored); current = {}; token.clear(); incoming = 0; SetNote(L"无法打开接收画面：" + error); return; }
        service.SetState(session::State::Busy); phase = Phase::PreparingReceive; phaseTime = Clock::now(); SetNote(L"正在打开接收画面。"); Publish();
    }
    void SaveTestResult() {
        std::ofstream out(dataDir / (testRole + L"-result.txt")); out << (testSucceeded ? "PASS" : "FAIL") << " role=" << Utf8(testRole) << " discovered=" << peers.size() << " connected=" << testConnected << '\n';
    }
    void Tick() {
        if (updater && updater->TakeExitRequest()) { SaveVolume(); closing = true; DestroyWindow(window); return; }
        if (updateReserved && updater && !updater->Busy()) { updateReserved = false; if (phase == Phase::Idle) service.SetState(session::State::Idle); }
        if (volumeSavePending && Clock::now() - volumeChangedAt >= std::chrono::milliseconds(400)) SaveVolume();
        ++ticks; worker.Poll(); if (uiReady && web.Ready()) ++readyTicks;
        if (!smoke) for (const auto& event : service.PollEvents()) {
            if (event.kind == session::Event::Kind::IncomingRequest) BeginReceive(event);
            else if (event.peer.id == current.id && event.token == token && phase != Phase::Idle && phase != Phase::Stopping) StopSession(L"对方已断开连接。");
        }
        if (request.valid() && request.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto [ok, error] = request.get();
            if (phase == Phase::Requesting) {
                if (ok) { phase = Phase::Sending; phaseTime = Clock::now(); testConnected = true; service.SetState(session::State::Sending); SetNote({}); }
                else StopSession(L"连接未成功：" + Wide(error));
            }
        }
        if (phase == Phase::PreparingSend && worker.Ready()) {
            std::wsmatch match; const auto log = worker.TailLog();
            if (std::regex_search(log, match, std::wregex(L"Listening on [^:]+:(\\d+)"))) {
                const auto port = uint16_t(std::stoul(match[1].str())); auto peer = current; auto sessionToken = token; phase = Phase::Requesting;
                request = std::async(std::launch::async, [this, peer, sessionToken, port] { std::string error; bool ok = service.RequestReceive(peer, sessionToken, port, 10000, error); return std::make_pair(ok, error); });
            }
        }
        if (phase == Phase::PreparingReceive && worker.Ready()) {
            std::string error;
            if (service.Reply(incoming, true, {}, error)) { phase = Phase::Receiving; phaseTime = Clock::now(); testConnected = true; service.SetState(session::State::Receiving); SetNote({}); }
            else StopSession(L"连接请求已过期，请对方重新连接。");
        }
        if (phase != Phase::Idle && !worker.Running() && worker.ExitCode().has_value()) {
            if (phase != Phase::Stopping) {
                service.EndSession(current, token); SetNote(*worker.ExitCode() ? L"画面未能继续。请在设置中查看日志，然后重新连接。" : L"连接已结束。");
            }
            if (!request.valid()) { phase = Phase::Idle; service.SetState(session::State::Idle); current = {}; token.clear(); incoming = 0; contentKind = "desktop"; encodingSummary.clear(); preview.clear(); receivedPreview.clear(); videoRecovering = false; }
        }
        if ((phase == Phase::PreparingSend || phase == Phase::PreparingReceive) && Clock::now() - phaseTime > std::chrono::seconds(45)) StopSession(L"准备画面超时，正在取消。");
        if (worker.StopOverdue()) SetNote(L"采集正在完成清理，请稍候。");
        if (ticks % 5 == 0) {
            if (phase == Phase::Sending || phase == Phase::Receiving) UpdateEncodingNote();
            if (!smoke) RefreshPeers();
            if (!smoke && testRole.empty() && phase == Phase::Idle && IsWindowVisible(window) && !IsIconic(window)) {
                const auto source = std::find_if(sources.begin(), sources.end(), [&](const Source& s) { return s.id == selectedSource && s.kind == Source::Screen; });
                if (source != sources.end() && source->monitor < displays.size()) preview = Preview(displays[source->monitor]); else preview.clear();
            }
            if (phase == Phase::Sending || phase == Phase::Receiving) {
                PreviewImage thumbnail;
                if (ReadPreview(worker.ProcessId(), thumbnail)) {
                    contentKind = thumbnail.frame.kind == ContentKind::LeiaSbs ? "sbs" : "desktop";
                    (phase == Phase::Sending ? preview : receivedPreview) = Preview(thumbnail.frame);
                }
            }
        }
        Publish();
        if (pendingSettings && phase == Phase::Idle && uiReady) { pendingSettings = false; web.Post(Json{{"type", "openSettings"}}.dump()); }
        if (smoke && readyTicks >= 5 && !smokeCaptured) {
            smokeCaptured = true; web.Capture(smokePath, [this](bool ok) { testSucceeded = ok; PostMessageW(window, WM_CLOSE, 0, 0); });
        }
        if ((smoke || !testRole.empty()) && !uiReady && Clock::now() - testStart > std::chrono::seconds(15)) { uiFailed = true; DestroyWindow(window); return; }
        if (!testRole.empty()) {
            if (testRole == L"sender" && !testStarted && !peers.empty() && phase == Phase::Idle && readyTicks >= 2) {
                selectedSource = testDesktop ? sources.front().id : "pattern"; Publish(); testStarted = true;
                // Exercise the same embedded page button and message bridge used by a person.
                web.Execute(L"document.getElementById('connect')?.click();");
            }
            if (testRole == L"sender" && phase == Phase::Sending && Clock::now() - phaseTime > std::chrono::seconds(3)) StopSession(L"测试完成。");
            if (testConnected && phase == Phase::Idle) { testSucceeded = true; SaveTestResult(); DestroyWindow(window); return; }
            if (Clock::now() - testStart > std::chrono::seconds(24)) { SaveTestResult(); StopSession(L"测试超时。"); DestroyWindow(window); return; }
        }
        if (closing && !worker.Running() && !request.valid()) DestroyWindow(window);
    }
    static LRESULT CALLBACK Proc(HWND h, UINT message, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->window = h; SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self)); }
        if (!self) return DefWindowProcW(h, message, wp, lp);
        if (message == ViewerVolumeChangedMessage() && self->phase == Phase::Receiving) {
            DWORD pid{}; GetWindowThreadProcessId(reinterpret_cast<HWND>(wp), &pid);
            wchar_t cls[128]{}; GetClassNameW(reinterpret_cast<HWND>(wp), cls, 128);
            if (pid == self->worker.ProcessId() && !wcscmp(cls, ViewerWindowClass) && lp >= 0 && lp <= 800) {
                self->SetReceiverVolume(int(lp), false); self->Publish();
            }
            return 0;
        }
        if (message == ViewerSettingsMessage() && self->phase == Phase::Receiving) {
            DWORD pid{}; GetWindowThreadProcessId(reinterpret_cast<HWND>(wp), &pid);
            if (pid == self->worker.ProcessId()) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); self->pendingSettings = true; self->StopSession(L"正在结束共享，以调整显示设置。"); } return 0;
        }
        switch (message) {
        case WM_CREATE: {
            const DWORD rounded = 2; DwmSetWindowAttribute(h, 33, &rounded, sizeof(rounded));
            self->RefreshSources(); SetTimer(h, 1, 200, nullptr);
            self->web.Open(h, self->dataDir / (L"webview-" + (self->testRole.empty() ? L"ui" : self->testRole)), false,
                [self](const std::string& json) { self->OnMessage(json); }, [self](const std::string& error) {
                    self->uiFailed = true; if (self->testRole.empty() && !self->smoke) MessageBoxW(self->window, Wide(error).c_str(), L"LAN3DShare", MB_OK | MB_ICONERROR); PostMessageW(self->window, WM_CLOSE, 0, 0);
                }); return 0;
        }
        case WM_NCCALCSIZE: if (wp) return 0; break;
        case WM_SIZE: case WM_MOVE: self->web.Resize(); return 0;
        case WM_GETMINMAXINFO: {
            auto m = reinterpret_cast<MINMAXINFO*>(lp); m->ptMinTrackSize = {self->Scale(780), self->Scale(540)};
            MONITORINFO info{sizeof(info)}; if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &info)) {
                m->ptMaxPosition = {info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top}; m->ptMaxSize = {info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top};
            } return 0;
        }
        case WM_DPICHANGED: { auto r = reinterpret_cast<RECT*>(lp); SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); self->web.Resize(); return 0; }
        case WM_TIMER: self->Tick(); return 0;
        case WM_ERASEBKGND: { RECT r{}; GetClientRect(h, &r); auto brush = CreateSolidBrush(RGB(248, 249, 251)); FillRect(HDC(wp), &r, brush); DeleteObject(brush); return 1; }
        case WM_CLOSE: self->SaveVolume(); self->closing = true; if (self->phase != Phase::Idle) self->StopSession(L"正在结束共享…"); else DestroyWindow(h); return 0;
        case WM_DESTROY: KillTimer(h, 1); self->web.Close(); PostQuitMessage(0); return 0;
        } return DefWindowProcW(h, message, wp, lp);
    }
};
struct Apartment { HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)}; ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); } };
struct Imaging { ULONG_PTR token{}; Imaging() { Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token, &input, nullptr); } ~Imaging() { if (token) Gdiplus::GdiplusShutdown(token); } };
}
int RunApplication(HINSTANCE instance, int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); Apartment apartment; Imaging imaging;
    App app; app.instance = instance;
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, DWORD(std::size(path))); app.executable = path;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i]; auto next = [&]() { if (++i >= argc) throw std::runtime_error("Missing UI test option"); return std::wstring(argv[i]); };
        if (arg == L"--ui-smoke") { app.smoke = true; app.smokePath = next(); }
        else if (arg == L"--ui-smoke-state") app.smokeState = Utf8(next());
        else if (arg == L"--ui-test") app.testRole = next();
        else if (arg == L"--test-discovery-port") app.testPort = uint16_t(std::stoul(next()));
        else if (arg == L"--test-target-port") app.testTarget = uint16_t(std::stoul(next()));
        else if (arg == L"--test-results") app.dataDir = next();
        else if (arg == L"--test-desktop") app.testDesktop = true;
    }
    HANDLE singleton{};
    if (app.smoke || !app.testRole.empty()) {
        if (app.dataDir.empty()) app.dataDir = app.smokePath.parent_path();
        if (app.dataDir.empty()) app.dataDir = std::filesystem::current_path();
        std::filesystem::create_directories(app.dataDir);
        app.settings.name = app.smoke ? L"PureDark-Gaming" : app.testRole == L"sender" ? L"测试发送端" : L"测试接收端"; app.settings.id = Guid();
    } else {
        app.dataDir = DataDirectory(); app.settings.Load(app.dataDir);
        singleton = CreateMutexW(nullptr, FALSE, (L"Local\\LAN3DShare.UI." + Wide(app.settings.id)).c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) { if (auto h = FindWindowW(L"LAN3DShareMain", nullptr)) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); } CloseHandle(singleton); return 0; }
    }
    if (!app.smoke && app.testRole.empty()) app.updater = std::make_unique<update::Client>(app.executable, app.dataDir);
    app.StartService();
    WNDCLASSW c{}; c.hInstance = instance; c.lpfnWndProc = App::Proc; c.lpszClassName = L"LAN3DShareMain"; c.hCursor = LoadCursorW(nullptr, IDC_ARROW); c.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101)); RegisterClassW(&c);
    const UINT dpi = GetDpiForSystem();
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    auto h = CreateWindowExW(0, c.lpszClassName, L"LAN3DShare", style, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(780, dpi, 96), MulDiv(540, dpi, 96), nullptr, nullptr, instance, &app);
    if (!h) { app.service.Stop(); if (singleton) CloseHandle(singleton); return 1; }
    if (app.testRole.empty()) ShowWindow(h, app.smoke ? SW_SHOWNOACTIVATE : SW_SHOW);
    MSG m{}; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (!app.token.empty()) app.service.EndSession(app.current, app.token);
    app.worker.RequestStop(); app.service.Stop(); if (app.request.valid()) app.request.wait(); app.web.Close();
    if (singleton) CloseHandle(singleton);
    return app.uiFailed || ((app.smoke || !app.testRole.empty()) && !app.testSucceeded) ? 1 : 0;
}
}
