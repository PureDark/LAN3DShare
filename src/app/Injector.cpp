#include "Injector.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <algorithm>
#include <chrono>

namespace lan3d {
namespace {
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
std::string Error(const char* operation) { return std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ")"; }
std::wstring NormalizedPath(const std::wstring& value) {
    std::error_code ec;
    auto path = std::filesystem::canonical(value, ec);
    if (ec) { ec.clear(); path = std::filesystem::absolute(value, ec).lexically_normal(); }
    if (ec) return {};
    auto result = path.make_preferred().wstring();
    // Toolhelp normally returns DOS paths even if LoadLibrary received an
    // extended-length spelling of that same file.
    if (result.rfind(L"\\\\?\\UNC\\", 0) == 0) result = L"\\\\" + result.substr(8);
    else if (result.rfind(L"\\\\?\\", 0) == 0) result.erase(0, 4);
    return result;
}
uintptr_t ModuleBase(uint32_t pid, const std::wstring& name, bool exactPath = false) {
    Handle snapshot;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(1);
    }
    if (snapshot.value == INVALID_HANDLE_VALUE) return 0;
    const auto expected = exactPath ? NormalizedPath(name) : name;
    if (expected.empty()) return 0;
    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot.value, &entry)) do {
        const auto candidate = exactPath ? NormalizedPath(entry.szExePath) : std::wstring(entry.szModule);
        if (_wcsicmp(candidate.c_str(), expected.c_str()) == 0) return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot.value, &entry));
    return 0;
}
bool Check64(HANDLE process, std::string& error) {
    USHORT machine{}, native{};
    if (!IsWow64Process2(process, &machine, &native)) { error = Error("Cannot query target architecture"); return false; }
    if (machine != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_AMD64) {
        error = "Only native Windows x64 target processes are supported"; return false;
    }
    return true;
}
bool ReadRemote(HANDLE process, uintptr_t address, void* data, size_t bytes, std::string& error) {
    SIZE_T read{};
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), data, bytes, &read) || read != bytes) {
        error = Error("Cannot read launched executable image"); return false;
    }
    return true;
}
bool PatchByte(HANDLE process, uintptr_t address, uint8_t byte, std::string& error) {
    auto* target = reinterpret_cast<void*>(address);
    DWORD previous{};
    if (!VirtualProtectEx(process, target, 1, PAGE_EXECUTE_READWRITE, &previous)) {
        error = Error("Cannot protect temporary entry breakpoint"); return false;
    }
    SIZE_T written{};
    const bool write = WriteProcessMemory(process, target, &byte, 1, &written) && written == 1;
    const DWORD writeError = GetLastError();
    DWORD ignored{};
    const bool restored = VirtualProtectEx(process, target, 1, previous, &ignored) != FALSE;
    const DWORD protectError = GetLastError();
    if (!write) { SetLastError(writeError); error = Error("Cannot write temporary entry breakpoint"); return false; }
    if (!restored) { SetLastError(protectError); error = Error("Cannot restore entry point page protection"); return false; }
    if (!FlushInstructionCache(process, target, 1)) { error = Error("Cannot flush entry point instruction cache"); return false; }
    return true;
}
struct DebugLaunch {
    PROCESS_INFORMATION process{};
    DEBUG_EVENT event{};
    bool attached{}, pending{};
    ~DebugLaunch() {
        if (!process.hProcess) return;
        // This guard owns only a process created by LaunchForCapture. Never
        // terminate an existing attach target or free a live remote argument.
        TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
        if (pending) ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
        if (attached) DebugActiveProcessStop(process.dwProcessId);
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
};
bool Invoke(HANDLE process, uintptr_t function, const void* arg, size_t bytes, uint32_t& result, std::string& error) {
    void* remote = nullptr;
    if (bytes) {
        remote = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SIZE_T written{};
        if (!remote || !WriteProcessMemory(process, remote, arg, bytes, &written) || written != bytes) {
            error = Error("Cannot copy capture parameters");
            if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            return false;
        }
    }
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), remote, 0, nullptr));
    if (!thread.value) {
        error = Error("Cannot start capture bootstrap");
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    const auto wait = WaitForSingleObject(thread.value, 30000);
    if (wait != WAIT_OBJECT_0) {
        // The target may still access its parameters; never free them or terminate its thread here.
        error = "Capture bootstrap did not finish within 30 seconds; target was left running";
        return false;
    }
    DWORD code{};
    const auto ok = GetExitCodeThread(thread.value, &code);
    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (!ok) { error = Error("Cannot read capture bootstrap result"); return false; }
    result = code;
    return true;
}
DWORD ExportRva(const std::wstring& path, const char* name) {
    // Map the image without executing DllMain or resolving target dependencies.
    HMODULE image = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return 0;
    const auto address = GetProcAddress(image, name);
    const auto offset = address ? reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(image) : 0;
    FreeLibrary(image);
    return offset <= std::numeric_limits<DWORD>::max() ? static_cast<DWORD>(offset) : 0;
}
}
bool LaunchForCapture(const std::wstring& executable, const std::wstring& arguments,
    PROCESS_INFORMATION& output, std::string& error) {
    output = {}; error.clear();
    const auto path = NormalizedPath(executable);
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) { error = "Application executable does not exist"; return false; }
    std::wstring command = L"\"" + path + L"\"" + (arguments.empty() ? L"" : L" " + arguments);
    const auto directory = std::filesystem::path(path).parent_path().wstring();
    STARTUPINFOW startup{ sizeof(startup) };
    DebugLaunch launch;
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS,
        nullptr, directory.c_str(), &startup, &launch.process)) {
        error = Error("Cannot launch application under capture bootstrap"); return false;
    }
    launch.attached = true;
    if (!Check64(launch.process.hProcess, error)) return false;
    uintptr_t entry{};
    uint8_t original{};
    bool initialBreakpointSeen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const DWORD timeout = static_cast<DWORD>(std::clamp<int64_t>(remaining, 1, 100));
        if (!WaitForDebugEvent(&launch.event, timeout)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            error = Error("Cannot wait for executable entry point"); return false;
        }
        launch.pending = true;
        auto& event = launch.event;
        DWORD continuation = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            // hProcess/hThread belong to the debugger event machinery. Only
            // hFile is manually closed; PROCESS_INFORMATION has our handles.
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            const auto base = reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            if (!base || !ReadRemote(launch.process.hProcess, base, &dos, sizeof(dos), error)) return false;
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) {
                error = "Launched executable has invalid PE DOS headers"; return false;
            }
            if (!ReadRemote(launch.process.hProcess, base + static_cast<uint32_t>(dos.e_lfanew), &nt, sizeof(nt), error)) return false;
            if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                !nt.OptionalHeader.AddressOfEntryPoint || nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage ||
                base > std::numeric_limits<uintptr_t>::max() - nt.OptionalHeader.AddressOfEntryPoint) {
                error = "Capture launcher requires a native x64 PE executable with a valid entry point"; return false;
            }
            entry = base + nt.OptionalHeader.AddressOfEntryPoint;
            if (!ReadRemote(launch.process.hProcess, entry, &original, sizeof(original), error)) return false;
            if (original == 0xCC) { error = "Executable entry already contains a breakpoint"; return false; }
            if (!PatchByte(launch.process.hProcess, entry, 0xCC, error)) return false;
            break;
        }
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& exception = event.u.Exception;
            const auto address = reinterpret_cast<uintptr_t>(exception.ExceptionRecord.ExceptionAddress);
            if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && entry && address == entry) {
                if (event.dwThreadId != launch.process.dwThreadId) { error = "EXE entry point was reached on an unexpected thread"; return false; }
                if (!PatchByte(launch.process.hProcess, entry, original, error)) return false;
                CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
                if (!GetThreadContext(launch.process.hThread, &context)) { error = Error("Cannot read entry point thread context"); return false; }
                context.Rip = entry;
                if (!SetThreadContext(launch.process.hThread, &context)) { error = Error("Cannot restore entry point instruction pointer"); return false; }
                if (SuspendThread(launch.process.hThread) == DWORD(-1)) { error = Error("Cannot hold main thread for capture injection"); return false; }
                if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE)) { error = Error("Cannot continue entry point debug event"); return false; }
                launch.pending = false;
                if (!DebugActiveProcessStop(launch.process.dwProcessId)) { error = Error("Cannot detach capture bootstrap debugger"); return false; }
                launch.attached = false;
                output = launch.process;
                launch.process = {};
                return true;
            }
            if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && !initialBreakpointSeen) {
                // The loader's first breakpoint is earlier than EXE entry and
                // can still be under the loader lock. Continue to our INT3.
                initialBreakpointSeen = true;
            } else {
                if (!exception.dwFirstChance) {
                    error = "Application failed before EXE entry (exception " + std::to_string(exception.ExceptionRecord.ExceptionCode) + ")";
                    return false;
                }
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            error = "Application exited before EXE entry (code " + std::to_string(event.u.ExitProcess.dwExitCode) + ")";
            return false;
        default:
            break;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) { error = Error("Cannot continue capture bootstrap debug event"); return false; }
        launch.pending = false;
    }
    error = "Application did not reach its EXE entry point within 30 seconds";
    return false;
}
std::vector<ProcessInfo> ListProcesses() {
    std::vector<ProcessInfo> processes;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (entry.th32ProcessID && entry.th32ProcessID != GetCurrentProcessId())
            processes.push_back({entry.th32ProcessID, entry.szExeFile, ModuleBase(entry.th32ProcessID, L"SimulatedRealityDirectX.dll") != 0});
    } while (Process32NextW(snapshot.value, &entry));
    return processes;
}
bool InjectCapture(uint32_t pid, const std::wstring& path, std::string& error, const ProcessIdentity* expected) {
    error.clear();
    if (!pid || pid == GetCurrentProcessId()) { error = "Choose a separate x64 source process"; return false; }
    const auto normalized = NormalizedPath(path);
    std::error_code ec;
    if (normalized.empty() || !std::filesystem::is_regular_file(normalized, ec)) { error = "Capture DLL does not exist"; return false; }
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process.value) { error = Error("Cannot open source process"); return false; }
    if (expected && !CheckExpectedProcess(process.value, *expected, error)) return false;
    if (!Check64(process.value, error)) return false;
    if (ModuleBase(pid, normalized, true)) return true;
    const auto filename = std::filesystem::path(normalized).filename().wstring();
    if (ModuleBase(pid, filename)) { error = "A capture DLL from another folder is already loaded; restart the source application before changing builds"; return false; }
    const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner{};
    if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(load), &owner)) { error = Error("Cannot locate loader"); return false; }
    wchar_t ownerPath[32768]{};
    if (!GetModuleFileNameW(owner, ownerPath, _countof(ownerPath))) { error = Error("Cannot locate loader module"); return false; }
    const auto remoteOwner = ModuleBase(pid, std::filesystem::path(ownerPath).filename().wstring());
    if (!remoteOwner) { error = "Loader module unavailable in target"; return false; }
    const auto address = remoteOwner + reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(owner);
    uint32_t ignored{};
    if (!Invoke(process.value, address, normalized.c_str(), (normalized.size() + 1) * sizeof(wchar_t), ignored, error)) return false;
    if (!ModuleBase(pid, normalized, true)) { error = "Target could not load capture DLL (check runtime dependencies and application policy)"; return false; }
    return true;
}
bool CallCapture(uint32_t pid, const std::wstring& path, const char* name, const void* argument,
    size_t bytes, uint32_t& result, std::string& error, const ProcessIdentity* expected) {
    error.clear(); result = 0;
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process.value) { error = Error("Cannot open source process"); return false; }
    if (expected && !CheckExpectedProcess(process.value, *expected, error)) return false;
    if (!Check64(process.value, error)) return false;
    const auto normalized = NormalizedPath(path);
    const auto base = ModuleBase(pid, normalized, true);
    if (!base) { error = "Capture DLL is not loaded from the expected folder"; return false; }
    const auto rva = ExportRva(normalized, name);
    if (!rva) { error = std::string("Capture export missing: ") + name; return false; }
    return Invoke(process.value, base + rva, argument, bytes, result, error);
}
}
