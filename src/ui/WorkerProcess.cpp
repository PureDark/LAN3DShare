#include "WorkerProcess.h"
#include <ShlObj.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace lan3d::ui {
namespace {
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) Reset(other.Release()); return *this; }
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() { return std::exchange(value_, nullptr); }
    void Reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_{};
};
std::wstring ErrorText(DWORD code) {
    wchar_t* buffer{};
    const auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = count && buffer ? std::wstring(buffer, count) : L"Windows error";
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result + L" (" + std::to_wstring(code) + L")";
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring FromUtf8(const char* text, size_t bytes) {
    if (!bytes) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(bytes), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(bytes), result.data(), count);
    return result;
}
// The Microsoft CRT argv rules: quotes need an escaping backslash; preceding
// backslashes are doubled, as are trailing backslashes before the closing quote.
std::wstring Quote(const std::wstring& value) {
    std::wstring result(1, L'"'); size_t slashes{};
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += L'"'; }
        else { result.append(slashes, L'\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
bool EventArgument(const std::wstring& arg) {
    for (const auto option : {L"--stop-event", L"--ready-event"}) {
        const auto length = wcslen(option);
        if (arg == option || (arg.size() > length && arg.compare(0, length, option) == 0 && arg[length] == L'=')) return true;
    }
    return false;
}
bool UniqueId(std::wstring& result) {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) return false;
    wchar_t buffer[40]{}; if (!StringFromGUID2(id, buffer, 40)) return false;
    result = buffer; return true;
}
std::filesystem::path DefaultLogDirectory() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) return {};
    std::filesystem::path result(path); CoTaskMemFree(path); return result / L"LAN3DShare" / L"logs";
}
}

struct WorkerProcess::Impl {
    WorkerProcessOptions options;
    Handle process, stopEvent, readyEvent, log;
    WorkerMode mode{WorkerMode::Sender};
    DWORD processId{};
    std::optional<DWORD> exitCode;
    std::filesystem::path logPath;
    std::wstring state{L"未启动"};
    bool ready{}, stopRequested{}, overdueReported{};
    ULONGLONG stopRequestedAt{};
    explicit Impl(WorkerProcessOptions value) : options(value) { options.destructorWaitMs = (std::min)(options.destructorWaitMs, DWORD(5000)); }
    bool Running() const { return process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT; }
    void WriteLog(const std::wstring& message) noexcept {
        if (!log) return;
        try { const auto bytes = Utf8(L"\r\n[界面] " + message + L"\r\n"); DWORD written{}; WriteFile(log.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr); }
        catch (...) {}
    }
    void RequestStop() {
        if (!Running() || stopRequested) return;
        if (!SetEvent(stopEvent.Get())) { state = L"无法通知后台进程停止：" + ErrorText(GetLastError()); WriteLog(state); return; }
        stopRequested = true; stopRequestedAt = GetTickCount64(); state = L"正在等待后台进程结束";
        WriteLog(mode == WorkerMode::Sender ? L"已请求停止发送；源程序采集是否成功停止，以后台进程退出日志为准。" : L"已请求停止接收。");
    }
    void Poll() {
        if (!process) return;
        const auto wait = WaitForSingleObject(process.Get(), 0);
        if (wait == WAIT_OBJECT_0) {
            DWORD code{};
            if (GetExitCodeProcess(process.Get(), &code)) exitCode = code;
            state = L"后台进程已退出" + (exitCode ? L"（代码 " + std::to_wstring(*exitCode) + L"）" : L"");
            if (mode == WorkerMode::Sender && stopRequested) state += L"；采集停止结果见日志";
            WriteLog(state); process.Reset(); stopEvent.Reset(); readyEvent.Reset(); return;
        }
        if (wait == WAIT_FAILED) { state = L"无法查询后台进程状态：" + ErrorText(GetLastError()); return; }
        if (!ready && readyEvent && WaitForSingleObject(readyEvent.Get(), 0) == WAIT_OBJECT_0) {
            ready = true;
            if (!stopRequested) state = mode == WorkerMode::Sender ? L"发送服务已就绪" : L"接收进程已就绪";
        }
        if (stopRequested && GetTickCount64() - stopRequestedAt >= options.stopWarningAfterMs && !overdueReported) {
            overdueReported = true;
            state = mode == WorkerMode::Sender ? L"发送进程仍在停止，尚未确认源程序采集已停止" : L"接收进程仍在停止";
            WriteLog(state + L"；界面保持响应，不强制终止后台进程或源程序。");
        }
    }
    ~Impl() {
        if (!process) return;
        try { RequestStop(); } catch (...) { if (stopEvent) SetEvent(stopEvent.Get()); }
        if (WaitForSingleObject(process.Get(), options.destructorWaitMs) == WAIT_OBJECT_0) {
            try { Poll(); } catch (...) {}
        } else {
            WriteLog(mode == WorkerMode::Sender
                ? L"界面已关闭，发送进程仍在执行停止流程。未强退后台进程或源程序，尚未确认源程序采集已停止。"
                : L"界面已关闭，接收进程仍在执行停止流程，未强制终止。");
        }
        // No job object or TerminateProcess: a sender may have created a source
        // game, and neither that game nor an in-flight capture stop is killed.
    }
};

WorkerProcess::WorkerProcess(WorkerProcessOptions options) : p_(std::make_unique<Impl>(options)) {}
WorkerProcess::~WorkerProcess() = default;
bool WorkerProcess::Start(const std::filesystem::path& executable, WorkerMode mode,
    const std::vector<std::wstring>& arguments, std::wstring& error, const std::filesystem::path& logDirectory) {
    error.clear(); p_->Poll();
    if (p_->Running()) { error = L"后台进程仍在运行或停止中"; return false; }
    if (mode != WorkerMode::Sender && mode != WorkerMode::Receiver) { error = L"无效的后台进程类型"; return false; }
    try {
        for (const auto& arg : arguments) {
            if (arg.find(L'\0') != std::wstring::npos) { error = L"参数包含无法传递的空字符"; return false; }
            if (EventArgument(arg)) { error = L"停止与就绪事件由界面管理，不能通过额外参数覆盖"; return false; }
        }
        if (executable.native().find(L'\0') != std::wstring::npos) { error = L"程序路径包含空字符"; return false; }
        std::error_code ec; const auto exe = std::filesystem::absolute(executable, ec);
        if (ec || !std::filesystem::is_regular_file(exe, ec)) { error = L"找不到后台程序：" + executable.native(); return false; }
        auto directory = logDirectory.empty() ? DefaultLogDirectory() : std::filesystem::absolute(logDirectory, ec);
        if (ec || directory.empty()) { error = L"无法确定日志目录"; return false; }
        std::filesystem::create_directories(directory, ec);
        if (ec) { error = L"无法创建日志目录：" + directory.native(); return false; }
        std::wstring id; Handle stop, ready;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!UniqueId(id)) { error = L"无法创建后台会话标识"; return false; }
            const auto base = L"Local\\LAN3DShare.worker." + id;
            stop.Reset(CreateEventW(nullptr, TRUE, FALSE, (base + L".stop").c_str()));
            if (!stop) { error = L"无法创建停止事件：" + ErrorText(GetLastError()); return false; }
            if (GetLastError() == ERROR_ALREADY_EXISTS) { stop.Reset(); continue; }
            ready.Reset(CreateEventW(nullptr, TRUE, FALSE, (base + L".ready").c_str()));
            if (!ready) { error = L"无法创建就绪事件：" + ErrorText(GetLastError()); return false; }
            if (GetLastError() == ERROR_ALREADY_EXISTS) { stop.Reset(); ready.Reset(); continue; }
            break;
        }
        if (!stop || !ready) { error = L"无法创建独立的后台控制事件"; return false; }
        const auto base = L"Local\\LAN3DShare.worker." + id;
        std::wstring command = Quote(exe.native()) + L" " + Quote(mode == WorkerMode::Sender ? L"--internal-send" : L"--internal-receive");
        for (const auto& arg : arguments) command += L" " + Quote(arg);
        command += L" --stop-event " + Quote(base + L".stop") + L" --ready-event " + Quote(base + L".ready");
        if (command.size() >= 32767) { error = L"后台进程参数过长"; return false; }
        SYSTEMTIME time{}; GetSystemTime(&time); wchar_t stamp[40]{};
        swprintf_s(stamp, L"%04u%02u%02uT%02u%02u%02uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        const auto logPath = directory / (std::wstring(stamp) + (mode == WorkerMode::Sender ? L"-send-" : L"-receive-") + id + L".log");
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        Handle log(CreateFileW(logPath.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!log) { error = L"无法创建日志文件：" + ErrorText(GetLastError()); return false; }
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!input) { error = L"无法创建后台输入：" + ErrorText(GetLastError()); return false; }
        SIZE_T size{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<std::byte> storage(size);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) { error = ErrorText(GetLastError()); return false; }
        struct AttributeGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeGuard() { DeleteProcThreadAttributeList(value); } } guard{attributes};
        const HANDLE inherited[]{input.Get(), log.Get()};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inherited), sizeof(inherited), nullptr, nullptr)) {
            error = L"无法限制后台继承句柄：" + ErrorText(GetLastError()); return false;
        }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.Get(); startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.Get(); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        // CREATE_NO_WINDOW hides the console. Do not set STARTF_USESHOWWINDOW /
        // SW_HIDE: doing that would also suppress the receiver's actual window.
        const bool created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &process) != FALSE;
        if (!created) { error = L"无法启动后台程序：" + ErrorText(GetLastError()); return false; }
        CloseHandle(process.hThread);
        p_->process.Reset(process.hProcess); p_->processId = process.dwProcessId; p_->stopEvent = std::move(stop); p_->readyEvent = std::move(ready); p_->log = std::move(log);
        p_->logPath = logPath; p_->mode = mode; p_->exitCode.reset(); p_->ready = p_->stopRequested = p_->overdueReported = false; p_->stopRequestedAt = 0;
        p_->state = L"正在启动后台进程"; p_->WriteLog(L"已启动后台进程 PID " + std::to_wstring(process.dwProcessId) + L"；本日志为 UTF-8。");
        return true;
    } catch (const std::exception& e) { error = L"启动后台进程失败：" + FromUtf8(e.what(), strlen(e.what())); return false; }
}
void WorkerProcess::RequestStop() { p_->RequestStop(); }
void WorkerProcess::Poll() { p_->Poll(); }
bool WorkerProcess::Ready() const { return p_->Running() && (p_->ready || (p_->readyEvent && WaitForSingleObject(p_->readyEvent.Get(), 0) == WAIT_OBJECT_0)); }
bool WorkerProcess::Running() const { return p_->Running(); }
bool WorkerProcess::StopRequested() const { return p_->stopRequested; }
bool WorkerProcess::StopOverdue() const { return p_->stopRequested && p_->Running() && GetTickCount64() - p_->stopRequestedAt >= p_->options.stopWarningAfterMs; }
DWORD WorkerProcess::ProcessId() const { return p_->processId; }
std::optional<DWORD> WorkerProcess::ExitCode() const {
    if (p_->process && WaitForSingleObject(p_->process.Get(), 0) == WAIT_OBJECT_0) { DWORD code{}; if (GetExitCodeProcess(p_->process.Get(), &code)) return code; }
    return p_->exitCode;
}
std::filesystem::path WorkerProcess::LogPath() const { return p_->logPath; }
std::wstring WorkerProcess::Status() const { return p_->state; }
std::wstring WorkerProcess::TailLog(size_t maxCharacters) const {
    if (!maxCharacters || p_->logPath.empty()) return {};
    maxCharacters = (std::min)(maxCharacters, size_t(65536));
    Handle file(CreateFileW(p_->logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return {};
    LARGE_INTEGER size{}; if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart <= 0) return {};
    const auto count = static_cast<DWORD>((std::min)(static_cast<uint64_t>(size.QuadPart), uint64_t(maxCharacters * 4 + 4)));
    LARGE_INTEGER start{}; start.QuadPart = size.QuadPart - count; if (!SetFilePointerEx(file.Get(), start, nullptr, FILE_BEGIN)) return {};
    std::vector<char> bytes(count); DWORD read{}; if (!ReadFile(file.Get(), bytes.data(), count, &read, nullptr) || !read) return {};
    size_t skip{}; if (start.QuadPart) while (skip < read && (static_cast<unsigned char>(bytes[skip]) & 0xc0) == 0x80) ++skip;
    auto text = FromUtf8(bytes.data() + skip, read - skip);
    if (!text.empty() && text.front() == 0xfeff) text.erase(0, 1);
    if (text.size() > maxCharacters) { text.erase(0, text.size() - maxCharacters); if (!text.empty() && text.front() >= 0xdc00 && text.front() <= 0xdfff) text.erase(0, 1); }
    return text;
}
}
