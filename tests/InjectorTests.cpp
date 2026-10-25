#include "app/Injector.h"
#include "capture/CaptureApi.h"
#include "capture/NativeGpuHooks.h"
#include "common/FrameBus.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>

void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) try {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") return 0;
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, _countof(exe));
    const auto capture = (std::filesystem::path(exe).parent_path() / L"LAN3DCapture.dll").wstring();
    struct Child {
        PROCESS_INFORMATION info{};
        ~Child() {
            if (info.hProcess) {
                if (WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(info.hProcess, 99);
                CloseHandle(info.hProcess); CloseHandle(info.hThread);
            }
        }
    } child;
    std::string error;
    lan3d::NativeGpuHooks gpuHooks;
    if (!lan3d::ProbeNativeGpuHooks(gpuHooks, error)) throw std::runtime_error("GPU probe in parent helper: " + error);
    if (!lan3d::LaunchForCapture(exe, L"--child", child.info, error)) throw std::runtime_error("Launch: " + error);
    auto slashAlias = capture;
    std::replace(slashAlias.begin(), slashAlias.end(), L'\\', L'/');
    const auto directory = std::filesystem::path(capture).parent_path();
    const auto dotAlias = (directory / L".." / directory.filename() / L"LAN3DCapture.dll").wstring();
    if (!lan3d::InjectCapture(child.info.dwProcessId, slashAlias, error)) throw std::runtime_error("Inject slash alias: " + error);
    uint32_t result{};
    lan3d::CaptureConfig config;
    for (const auto& alias : { capture, slashAlias, dotAlias }) {
        if (!lan3d::InjectCapture(child.info.dwProcessId, alias, error)) throw std::runtime_error("Repeat inject alias: " + error);
        if (!lan3d::CallCapture(child.info.dwProcessId, alias, "LAN3DConfigure", &config, sizeof(config), result, error))
            throw std::runtime_error("Call alias: " + error);
        Check(result == ERROR_SUCCESS, "Alias configuration failed");
    }
    auto call = [&](const char* name, const void* value = nullptr, size_t bytes = 0) {
        if (!lan3d::CallCapture(child.info.dwProcessId, capture, name, value, bytes, result, error)) throw std::runtime_error(std::string(name) + ": " + error);
        Check(result == ERROR_SUCCESS, std::string(name) + " returned " + std::to_string(result));
    };
    call("LAN3DConfigure", &config, sizeof(config));
    call("LAN3DSetGpuHooks", &gpuHooks, sizeof(gpuHooks));
    call("LAN3DStart");
    lan3d::FrameBus bus;
    if (!bus.Open(child.info.dwProcessId, false, error)) throw std::runtime_error("FrameBus: " + error);
    call("LAN3DStop");
    call("LAN3DStart");
    call("LAN3DStop");
    Check(ResumeThread(child.info.hThread) == 1, "Launched main thread did not have exactly one suspend count");
    Check(WaitForSingleObject(child.info.hProcess, 10000) == WAIT_OBJECT_0, "Child failed to exit");
    DWORD code{};
    Check(GetExitCodeProcess(child.info.hProcess, &code) && code == 0, "Child returned failure");
    std::cout << "PASS EXE entry bootstrap -> slash/dot alias injection -> configure -> start/stop/restart -> resume/exit\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
