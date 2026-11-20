#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#include <array>
#include <iostream>
#include <vector>
#include <string>

// Diagnostic only: never attaches a debugger or injects code. Each thread is
// resumed immediately after copying its context and bounded stack memory.
namespace {
DWORD64 stackBase{};
std::vector<unsigned char> stackBytes;
BOOL CALLBACK ReadMemory(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
    if (address >= stackBase && address - stackBase <= stackBytes.size() && size <= stackBytes.size() - (address - stackBase)) {
        memcpy(buffer, stackBytes.data() + (address - stackBase), size); *read = size; return TRUE;
    }
    SIZE_T count{};
    const auto ok = ReadProcessMemory(process, reinterpret_cast<void*>(address), buffer, size, &count);
    *read = static_cast<DWORD>(count); return ok;
}
void Address(HANDLE process, DWORD64 address) {
    IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
    std::cout << "0x" << std::hex << address;
    if (SymGetModuleInfo64(process, address, &module))
        std::cout << " " << module.ModuleName << "+0x" << address - module.BaseOfImage;
    alignas(SYMBOL_INFO) std::array<unsigned char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> bytes{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(bytes.data()); symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement{};
    if (SymFromAddr(process, address, &displacement, symbol)) std::cout << " " << symbol->Name << "+0x" << displacement;
    std::cout << std::dec;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "ThreadStacks PID local-symbol-directory\n"; return 1; }
    const DWORD pid = std::stoul(argv[1]);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) { std::cerr << "OpenProcess " << GetLastError(); return 1; }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(process, argv[2], TRUE)) { std::cerr << "SymInitialize " << GetLastError(); CloseHandle(process); return 1; }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)};
    std::vector<DWORD> threads;
    if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == pid) threads.push_back(entry.th32ThreadID); } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    stackBytes.resize(64 * 1024);
    for (DWORD id : threads) {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
        stackBytes.resize(64 * 1024); SIZE_T copied{};
        const auto previous = SuspendThread(thread);
        if (previous == DWORD(-1)) { CloseHandle(thread); continue; }
        const bool okay = GetThreadContext(thread, &context) != FALSE;
        stackBase = context.Rsp;
        if (okay) ReadProcessMemory(process, reinterpret_cast<void*>(stackBase), stackBytes.data(), stackBytes.size(), &copied);
        ResumeThread(thread);
        stackBytes.resize(copied);
        std::cout << "THREAD " << id << " prior_suspend=" << previous << " stack_bytes=" << copied;
        PWSTR name{};
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name) { std::wcout << L" name=" << name; LocalFree(name); }
        std::cout << '\n';
        if (okay) {
            STACKFRAME64 frame{}; frame.AddrPC = {context.Rip, 0, AddrModeFlat}; frame.AddrStack = {context.Rsp, 0, AddrModeFlat}; frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
            for (unsigned n = 0; n < 48 && frame.AddrPC.Offset; ++n) {
                std::cout << "  #" << n << " "; Address(process, frame.AddrPC.Offset); std::cout << '\n';
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, ReadMemory, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            }
        }
        CloseHandle(thread); std::cout << std::flush;
    }
    SymCleanup(process); CloseHandle(process); return 0;
}
