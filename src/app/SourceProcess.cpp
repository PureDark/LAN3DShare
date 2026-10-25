#include "SourceProcess.h"
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <utility>

namespace lan3d {
namespace {
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
std::string Error(const char* operation) {
    return std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ")";
}
std::wstring CanonicalPath(const std::wstring& value) {
    // Resolve junctions, symlinks and short/extended spellings to the same file
    // path. No data access or exclusive sharing is required for the image.
    Handle file(CreateFileW(value.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return {};
    const auto required = GetFinalPathNameByHandleW(file.value, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required || required > 32768) return {};
    std::wstring path(required, L'\0');
    const auto length = GetFinalPathNameByHandleW(file.value, path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= required) return {};
    path.resize(length);
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    return path;
}
bool SamePath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() &&
        CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool Alive(HANDLE process) {
    return process && process != INVALID_HANDLE_VALUE && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}
}

bool ReadProcessIdentity(HANDLE process, ProcessIdentity& identity, std::string& error) {
    identity = {}; error.clear();
    const auto pid = GetProcessId(process);
    if (!pid) { error = Error("Cannot query source process ID"); return false; }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        error = Error("Cannot query source process creation time"); return false;
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        error = Error("Cannot query source executable path"); return false;
    }
    path.resize(length);
    path = CanonicalPath(path);
    if (path.empty()) { error = Error("Cannot resolve source executable path"); return false; }
    identity = {pid, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime, std::move(path)};
    return true;
}

bool CheckExpectedProcess(HANDLE process, const ProcessIdentity& expected, std::string& error) {
    error.clear();
    if (!expected.pid || !expected.creationTime || expected.canonicalPath.empty()) {
        error = "Source process identity is incomplete"; return false;
    }
    if (!Alive(process)) { error = "Source process has exited or cannot be synchronized"; return false; }
    ProcessIdentity actual;
    if (!ReadProcessIdentity(process, actual, error)) return false;
    if (actual.pid != expected.pid || actual.creationTime != expected.creationTime ||
        !SamePath(actual.canonicalPath, expected.canonicalPath)) {
        error = "Source process identity changed; refusing to use a reused PID or different executable"; return false;
    }
    if (!Alive(process)) { error = "Source process exited during identity validation"; return false; }
    return true;
}

std::vector<ProcessIdentity> FindMatchingProcesses(const std::wstring& executable, std::string& error) {
    error.clear();
    const auto expected = CanonicalPath(executable);
    std::error_code ec;
    if (expected.empty() || !std::filesystem::is_regular_file(expected, ec)) {
        error = "Source executable path does not identify an accessible file"; return {};
    }
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) { error = Error("Cannot list source processes"); return {}; }
    PROCESSENTRY32W entry{sizeof(entry)};
    std::vector<ProcessIdentity> matches;
    if (!Process32FirstW(snapshot.value, &entry)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) error = Error("Cannot enumerate source processes");
        return matches;
    }
    do {
        if (!entry.th32ProcessID || entry.th32ProcessID == GetCurrentProcessId()) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!Alive(process.value)) continue;
        ProcessIdentity identity;
        std::string ignored;
        if (ReadProcessIdentity(process.value, identity, ignored) && SamePath(identity.canonicalPath, expected) && Alive(process.value))
            matches.push_back(std::move(identity));
    } while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        error = Error("Cannot complete source process enumeration"); return {};
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.pid < b.pid; });
    return matches;
}

PinnedProcess::~PinnedProcess() { if (handle_) CloseHandle(handle_); }
PinnedProcess::PinnedProcess(PinnedProcess&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), identity_(std::move(other.identity_)) {}
PinnedProcess& PinnedProcess::operator=(PinnedProcess&& other) noexcept {
    if (this != &other) {
        if (handle_) CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        identity_ = std::move(other.identity_);
    }
    return *this;
}
bool PinnedProcess::Alive() const { return lan3d::Alive(handle_); }

PinnedProcess OpenValidated(const ProcessIdentity& expected, std::string& error) {
    error.clear();
    PinnedProcess result;
    result.handle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, expected.pid);
    if (!result.handle_) { error = Error("Cannot open source process identity"); return {}; }
    if (!CheckExpectedProcess(result.handle_, expected, error)) return {};
    result.identity_ = expected;
    return result;
}
}
