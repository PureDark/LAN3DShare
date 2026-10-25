#include "app/SourceProcess.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::wstring OwnPath() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(length && length < path.size(), "Cannot read fixture executable path");
    path.resize(length);
    return path;
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle(const Handle&) = delete;
};
HANDLE Spawn(const std::wstring& path, const std::wstring& arguments) {
    auto command = L"\"" + path + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != FALSE, "Cannot start test-owned fixture");
    CloseHandle(process.hThread);
    return process.hProcess;
}
int FixtureWait(const wchar_t* stopName, const wchar_t* readyName) {
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stopName));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName));
    if (!stop.value || !ready.value || !SetEvent(ready.value)) return 2;
    return WaitForSingleObject(stop.value, 15000) == WAIT_OBJECT_0 ? 0 : 3;
}
int FixtureRelaunch(const wchar_t* gateName, const wchar_t* stopName, const wchar_t* readyName) {
    Handle gate(OpenEventW(SYNCHRONIZE, FALSE, gateName));
    if (!gate.value || WaitForSingleObject(gate.value, 10000) != WAIT_OBJECT_0) return 2;
    // Simulate Steam replacing the original startup PID with a new instance
    // of exactly the same executable. No DLL, graphics API or game is loaded.
    Handle child(Spawn(OwnPath(), std::wstring(L"--fixture-wait ") + stopName + L" " + readyName));
    return 0;
}
struct Fixtures {
    std::wstring tag = L"Local\\LAN3DSourceProcess_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::wstring stopName = tag + L"_stop";
    std::wstring gateName = tag + L"_gate";
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, stopName.c_str())};
    Handle gate{CreateEventW(nullptr, TRUE, FALSE, gateName.c_str())};
    std::vector<Handle> processes;
    std::filesystem::path copiedExe, copiedDirectory;
    ~Fixtures() {
        if (stop.value) SetEvent(stop.value);
        if (gate.value) SetEvent(gate.value);
        for (auto& process : processes) {
            if (WaitForSingleObject(process.value, 3000) == WAIT_TIMEOUT) {
                // Every handle in this list came from our own fixture launch.
                TerminateProcess(process.value, ERROR_PROCESS_ABORTED);
                WaitForSingleObject(process.value, 1000);
            }
        }
        std::error_code ignored;
        if (!copiedExe.empty()) std::filesystem::remove(copiedExe, ignored);
        if (!copiedDirectory.empty()) std::filesystem::remove(copiedDirectory, ignored);
    }
    Handle Ready(const wchar_t* suffix) {
        auto name = tag + suffix;
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        Require(ready.value != nullptr, "Cannot create fixture ready event");
        return ready;
    }
    HANDLE Start(const std::wstring& executable, const std::wstring& arguments) {
        processes.emplace_back(Spawn(executable, arguments));
        return processes.back().value;
    }
};
std::vector<lan3d::ProcessIdentity> Matches(const std::wstring& path) {
    std::string error;
    auto found = lan3d::FindMatchingProcesses(path, error);
    Require(error.empty(), error.c_str());
    return found;
}
void TestRelaunch() {
    const auto executable = OwnPath();
    Fixtures fixtures;
    Require(fixtures.stop.value && fixtures.gate.value, "Cannot create fixture coordination events");
    auto replacementReady = fixtures.Ready(L"_replacement");
    auto launcher = fixtures.Start(executable, L"--fixture-relaunch " + fixtures.gateName + L" " +
        fixtures.stopName + L" " + fixtures.tag + L"_replacement");
    std::string error;
    lan3d::ProcessIdentity original;
    Require(lan3d::ReadProcessIdentity(launcher, original, error), error.c_str());
    const auto initial = Matches(executable);
    Require(initial.size() == 1 && initial.front().pid == original.pid, "Existing same-path process must be found uniquely");
    auto pinned = lan3d::OpenValidated(initial.front(), error);
    Require(pinned && pinned.Alive(), "Cannot pin existing source identity");

    auto stale = original;
    ++stale.creationTime;
    Require(!lan3d::OpenValidated(stale, error) && !error.empty(), "Reused PID with different creation time was accepted");
    stale = original;
    ++stale.pid;
    Require(!lan3d::CheckExpectedProcess(launcher, stale, error), "Mismatched PID was accepted on held handle");
    stale = original;
    stale.canonicalPath += L".different";
    Require(!lan3d::CheckExpectedProcess(launcher, stale, error), "Mismatched executable path was accepted");
    Require(!lan3d::CheckExpectedProcess(launcher, {}, error), "Incomplete source identity was accepted");

    auto alias = (std::filesystem::path(executable).parent_path() / L"." / std::filesystem::path(executable).filename()).wstring();
    std::transform(alias.begin(), alias.end(), alias.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    Require(Matches(alias).size() == 1, "Case and dot path aliases did not resolve to the existing source");
    Require(Matches(L"\\\\?\\" + executable).size() == 1, "Extended path spelling did not resolve to the existing source");

    Require(SetEvent(fixtures.gate.value) != FALSE, "Cannot release test launcher");
    Require(WaitForSingleObject(launcher, 5000) == WAIT_OBJECT_0, "Test launcher did not exit");
    Require(!pinned.Alive(), "Exited launcher remained live through pinned handle");
    Require(!lan3d::CheckExpectedProcess(pinned.handle(), original, error), "Exited launcher identity was accepted");
    Require(WaitForSingleObject(replacementReady.value, 5000) == WAIT_OBJECT_0, "Same-path replacement did not become ready");
    const auto replacement = Matches(executable);
    Require(replacement.size() == 1 && replacement.front().pid != original.pid, "Exited launcher did not resolve to unique same-path replacement");
    auto replacementPinned = lan3d::OpenValidated(replacement.front(), error);
    Require(replacementPinned && replacementPinned.Alive(), "Cannot validate replacement source");
    const auto replacementPid = replacementPinned.identity().pid;
    auto moved = std::move(replacementPinned);
    Require(!replacementPinned && moved && moved.identity().pid == replacementPid, "Pinned source move did not transfer ownership");
    pinned = std::move(moved);
    Require(!moved && pinned.Alive() && pinned.identity().pid == replacementPid, "Pinned source move assignment retained old identity");

    // A same-named executable in another directory must never be selected.
    const auto directory = std::filesystem::temp_directory_path() /
        (L"LAN3DSourceProcess_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    Require(std::filesystem::create_directory(directory), "Cannot create unique fixture directory");
    fixtures.copiedDirectory = directory;
    fixtures.copiedExe = directory / std::filesystem::path(executable).filename();
    Require(std::filesystem::copy_file(executable, fixtures.copiedExe), "Cannot copy different-path fixture");
    auto decoyReady = fixtures.Ready(L"_decoy");
    auto decoy = fixtures.Start(fixtures.copiedExe.wstring(), L"--fixture-wait " + fixtures.stopName + L" " + fixtures.tag + L"_decoy");
    Require(WaitForSingleObject(decoyReady.value, 5000) == WAIT_OBJECT_0, "Different-path fixture did not start");
    const auto withoutDecoy = Matches(executable);
    Require(withoutDecoy.size() == 1 && withoutDecoy.front().pid == replacementPid, "Same filename at another path matched the source");
    const auto decoyMatch = Matches(fixtures.copiedExe.wstring());
    Require(decoyMatch.size() == 1 && decoyMatch.front().pid == GetProcessId(decoy), "Different-path fixture identity was not isolated");

    auto secondReady = fixtures.Ready(L"_second");
    auto second = fixtures.Start(executable, L"--fixture-wait " + fixtures.stopName + L" " + fixtures.tag + L"_second");
    Require(WaitForSingleObject(secondReady.value, 5000) == WAIT_OBJECT_0, "Second same-path fixture did not start");
    const auto ambiguous = Matches(executable);
    Require(ambiguous.size() == 2, "Multiple same-path instances must remain ambiguous");
    Require(std::any_of(ambiguous.begin(), ambiguous.end(), [&](const auto& p) { return p.pid == replacementPid; }) &&
        std::any_of(ambiguous.begin(), ambiguous.end(), [&](const auto& p) { return p.pid == GetProcessId(second); }),
        "Ambiguous identities did not describe both live instances");

    SetEvent(fixtures.stop.value);
    Require(WaitForSingleObject(pinned.handle(), 5000) == WAIT_OBJECT_0, "Replacement did not stop");
    Require(WaitForSingleObject(second, 5000) == WAIT_OBJECT_0, "Second instance did not stop");
    Require(WaitForSingleObject(decoy, 5000) == WAIT_OBJECT_0, "Decoy did not stop");
    Require(Matches(executable).empty(), "Exited instances were reported as live sources");
    Require(!lan3d::OpenValidated(pinned.identity(), error), "Exited replacement identity was reopened");
    Require(lan3d::FindMatchingProcesses(executable + L".missing", error).empty() && !error.empty(), "Missing executable was not diagnosed");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--fixture-wait") return FixtureWait(argv[2], argv[3]);
        if (argc == 5 && std::wstring(argv[1]) == L"--fixture-relaunch") return FixtureRelaunch(argv[2], argv[3], argv[4]);
        TestRelaunch();
        std::cout << "Source process identity, launcher replacement, path isolation and ambiguity passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
