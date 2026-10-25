#include "ui/WorkerProcess.h"
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using lan3d::ui::WorkerMode;
using lan3d::ui::WorkerProcess;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void Write(HANDLE handle, const std::string& text) { DWORD written{}; WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr); }
std::wstring Hex(const std::wstring& value) {
    std::wstring result; wchar_t text[8]{};
    for (wchar_t c : value) { swprintf_s(text, L"%04X", unsigned(c)); result += text; }
    return result;
}
std::string Ascii(const std::wstring& value) { return std::string(value.begin(), value.end()); }
int FakeWorker(int argc, wchar_t** argv) {
    std::wstring stopName, readyName; DWORD linger{};
    for (int i = 2; i < argc; ++i) {
        const std::wstring value = argv[i];
        if (value == L"--stop-event" && i + 1 < argc) { stopName = argv[++i]; continue; }
        if (value == L"--ready-event" && i + 1 < argc) { readyName = argv[++i]; continue; }
        if (value == L"--test-linger" && i + 1 < argc) { linger = wcstoul(argv[++i], nullptr, 10); continue; }
        if (value == L"--test-fail") { Write(GetStdHandle(STD_ERROR_HANDLE), "EXPECTED_FAILURE\n"); return 23; }
        Write(GetStdHandle(STD_OUTPUT_HANDLE), "ARG=" + Ascii(Hex(value)) + "\n");
    }
    if (GetConsoleWindow() != nullptr) return 24;
    Write(GetStdHandle(STD_OUTPUT_HANDLE), reinterpret_cast<const char*>(u8"UTF8=中文画面\n"));
    Write(GetStdHandle(STD_ERROR_HANDLE), "STDERR_CAPTURED\n");
    std::cout << "CRT_STDOUT_CAPTURED\n" << std::flush;
    std::cerr << "CRT_STDERR_CAPTURED\n" << std::flush;
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    if (!stop || !ready) return 25;
    SetEvent(ready); CloseHandle(ready);
    const auto wait = WaitForSingleObject(stop, 5000); CloseHandle(stop);
    if (wait != WAIT_OBJECT_0) return 26;
    if (linger) Sleep(linger);
    Write(GetStdHandle(STD_OUTPUT_HANDLE), "GRACEFUL_EXIT\n"); return 0;
}
template<class Predicate> void Until(Predicate predicate, const char* error, DWORD timeout = 3000) {
    const auto start = GetTickCount64();
    while (!predicate()) { if (GetTickCount64() - start >= timeout) throw std::runtime_error(error); Sleep(5); }
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && (wcscmp(argv[1], L"--internal-send") == 0 || wcscmp(argv[1], L"--internal-receive") == 0)) return FakeWorker(argc, argv);
    try {
        Check(argc == 2, "usage: WorkerProcessTests <workspace log directory>");
        wchar_t executable[32768]{}; Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "module path");
        const std::filesystem::path logs = argv[1]; std::filesystem::create_directories(logs);
        std::vector<std::filesystem::path> createdLogs;
        std::wstring error;
        WorkerProcess sender, receiver;
        const std::vector<std::wstring> args{L"", L"中文 路径", L"quote\"inside", L"trailing\\\\", L"back\\\"quote", L"; & $(never-run) | < >", L"line\nsecond"};
        Check(sender.Start(executable, WorkerMode::Sender, args, error, logs), "sender start");
        createdLogs.push_back(sender.LogPath());
        Check(receiver.Start(executable, WorkerMode::Receiver, {L"second instance"}, error, logs), "receiver start");
        createdLogs.push_back(receiver.LogPath());
        Until([&] { sender.Poll(); receiver.Poll(); return sender.Ready() && receiver.Ready(); }, "private ready events");
        const auto text = sender.TailLog(12000);
        for (const auto& arg : args) Check(text.find(L"ARG=" + Hex(arg) + L"\n") != std::wstring::npos, "CreateProcess argv quoting mismatch");
        Check(text.find(L"UTF8=中文画面") != std::wstring::npos && text.find(L"STDERR_CAPTURED") != std::wstring::npos, "UTF8/stdout/stderr capture");
        Check(text.find(L"CRT_STDOUT_CAPTURED") != std::wstring::npos && text.find(L"CRT_STDERR_CAPTURED") != std::wstring::npos, "Windows subsystem C++ stdout/stderr capture");
        Check(sender.TailLog(16).size() <= 16, "tail log bound");
        Check(!sender.Start(executable, WorkerMode::Sender, {}, error, logs), "started over running worker");
        const auto requestTime = GetTickCount64(); sender.RequestStop(); Check(GetTickCount64() - requestTime < 200, "RequestStop blocked");
        Until([&] { sender.Poll(); return !sender.Running(); }, "sender graceful stop");
        Check(sender.ExitCode() == 0 && sender.TailLog().find(L"GRACEFUL_EXIT") != std::wstring::npos, "sender exit code or flush");
        Check(receiver.Running(), "one worker's stop affected another worker"); receiver.RequestStop();
        Until([&] { receiver.Poll(); return !receiver.Running(); }, "receiver graceful stop"); Check(receiver.ExitCode() == 0, "receiver exit code");
        Check(!sender.Start(executable, WorkerMode::Sender, {L"--stop-event", L"remote-name"}, error, logs), "external stop event override accepted");
        Check(!sender.Start(executable, WorkerMode::Sender, {std::wstring(L"a\0b", 3)}, error, logs), "embedded NUL accepted");
        Check(sender.Start(executable, WorkerMode::Sender, {L"--test-fail"}, error, logs), "failure worker start");
        createdLogs.push_back(sender.LogPath());
        Until([&] { sender.Poll(); return !sender.Running(); }, "failed worker exit"); Check(sender.ExitCode() == 23 && !sender.Ready(), "failure misreported ready");
        HANDLE lingering{}; std::filesystem::path detachedLog;
        const auto before = GetTickCount64();
        {
            WorkerProcess slow({20, 50});
            Check(slow.Start(executable, WorkerMode::Sender, {L"--test-linger", L"450"}, error, logs), "slow start");
            Until([&] { slow.Poll(); return slow.Ready(); }, "slow ready");
            lingering = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, slow.ProcessId()); Check(lingering != nullptr, "slow process handle");
            detachedLog = slow.LogPath(); createdLogs.push_back(detachedLog); slow.RequestStop();
            Until([&] { slow.Poll(); return slow.StopOverdue(); }, "nonblocking stop overdue");
            Check(slow.Running() && !slow.ExitCode(), "slow worker falsely reported stopped");
        }
        Check(GetTickCount64() - before < 1000, "destructor exceeded its bound");
        Check(WaitForSingleObject(lingering, 0) == WAIT_TIMEOUT, "destructor force-killed own worker");
        Check(WaitForSingleObject(lingering, 3000) == WAIT_OBJECT_0, "detached worker did not finish stop");
        DWORD code{}; GetExitCodeProcess(lingering, &code); CloseHandle(lingering); Check(code == 0, "detached worker exit");
        // Failed runs retain diagnostic logs. Successful runs delete only files
        // created by this test, after every owning worker has exited.
        for (const auto& log : createdLogs) { std::error_code ignored; std::filesystem::remove(log, ignored); }
        std::cout << "Worker process: exact Windows argv, private events, hidden console, UTF8 logs, nonblocking stop, exit codes, bounded non-killing teardown PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
// Build this test with the Windows subsystem to verify the same redirected
// stdout/stderr behavior as the single GUI executable used in production.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const auto result = wmain(argc, argv); LocalFree(argv); return result;
}
