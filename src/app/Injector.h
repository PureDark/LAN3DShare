#pragma once
#include "SourceProcess.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lan3d {
struct ProcessInfo { uint32_t id{}; std::wstring name; bool leiaLoaded{}; };
std::vector<ProcessInfo> ListProcesses();
// Launch only this executable under a temporary debugger, then detach at its
// EXE entry point. Loader initialization is complete and the main thread has
// one explicit suspend count. Caller owns both handles and must ResumeThread.
// Failure terminates only the process created by this call and clears output.
bool LaunchForCapture(const std::wstring& executable, const std::wstring& arguments,
    PROCESS_INFORMATION& process, std::string& error);
bool InjectCapture(uint32_t pid, const std::wstring& dllPath, std::string& error,
    const ProcessIdentity* expected = nullptr);
bool CallCapture(uint32_t pid, const std::wstring& dllPath, const char* exportName,
    const void* argument, size_t argumentBytes, uint32_t& result, std::string& error,
    const ProcessIdentity* expected = nullptr);
}
