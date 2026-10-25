#include "capture/CaptureApi.h"
#include "capture/NativeGpuHooks.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <iostream>
#include <stdexcept>

namespace {
constexpr wchar_t MissingModule[] = L"LAN3DShare.LoaderRegression.Absent.dll";
using ModuleQuery = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
ModuleQuery originalQuery{};
HANDLE fixtureEntered{}, workerQuery{}, nestedReturned{};
std::atomic<bool> armed{};
std::atomic<DWORD> queryThread{};
std::atomic<void*> queryCaller{};
std::atomic<bool> queryWaitFailed{};
volatile LONG presentCalls{};

void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> F Resolve(HMODULE module, const char* name) {
    auto function = reinterpret_cast<F>(GetProcAddress(module, name));
    Check(function != nullptr, name); return function;
}
BOOL WINAPI ObserveModuleQuery(DWORD flags, LPCWSTR name, HMODULE* module) {
    if (armed.load() && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) && name && !wcscmp(name, MissingModule)) {
        // The production scanner owns controlMutex at this call site. Keep
        // that query pending until the REAL nested LoadLibrary has returned,
        // rather than assuming GetModuleHandleEx itself takes the loader lock.
        queryThread = GetCurrentThreadId(); queryCaller = _ReturnAddress();
        SetEvent(workerQuery);
        if (WaitForSingleObject(nestedReturned, 10000) != WAIT_OBJECT_0) queryWaitFailed = true;
    }
    return originalQuery(flags, name, module);
}
__declspec(noinline) HRESULT WINAPI UnusedPresent(void*, UINT, UINT) {
    // A distinct, patchable CPU function satisfies the validated native hook
    // profile without creating any graphics object or invoking Present.
    return static_cast<HRESULT>(InterlockedIncrement(&presentCalls));
}
struct FixtureLoad { const wchar_t* path{}; bool loaded{}; };
DWORD WINAPI LoadFixture(void* value) {
    auto& load = *static_cast<FixtureLoad*>(value);
    const auto module = LoadLibraryW(load.path);
    load.loaded = module != nullptr;
    if (module) FreeLibrary(module);
    return 0;
}
HANDLE NamedEvent(const wchar_t* suffix) {
    wchar_t name[128]{};
    swprintf_s(name, L"Local\\LAN3DCaptureLoader_%lu_%ls", GetCurrentProcessId(), suffix);
    const auto event = CreateEventW(nullptr, TRUE, FALSE, name);
    Check(event != nullptr, "Cannot create loader test event"); return event;
}
}

int wmain(int argc, wchar_t** argv) try {
    Check(argc == 3, "Usage: CaptureLoaderTests capture.dll fixture.dll");
    fixtureEntered = NamedEvent(L"entered");
    const auto proceed = NamedEvent(L"continue");
    workerQuery = NamedEvent(L"query");
    nestedReturned = NamedEvent(L"nested_returned");
    Check(MH_Initialize() == MH_OK, "Cannot initialize CPU test query observer");
    const auto queryAddress = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetModuleHandleExW");
    Check(MH_CreateHook(reinterpret_cast<void*>(queryAddress), reinterpret_cast<void*>(&ObserveModuleQuery),
        reinterpret_cast<void**>(&originalQuery)) == MH_OK, "Cannot observe loader queries");
    Check(MH_EnableHook(reinterpret_cast<void*>(queryAddress)) == MH_OK, "Cannot enable loader query observer");

    const auto capture = LoadLibraryW(argv[1]); Check(capture != nullptr, "Cannot load capture under test");
    using Call = DWORD(WINAPI*)(void*);
    auto setHooks = Resolve<Call>(capture, "LAN3DSetGpuHooks");
    auto configure = Resolve<Call>(capture, "LAN3DConfigure");
    auto start = Resolve<Call>(capture, "LAN3DStart");
    auto stop = Resolve<Call>(capture, "LAN3DStop");
    // Use exactly the kernel32 export addresses installed by CaptureHooks,
    // and verify they really changed instead of trusting an IAT/API-set alias.
    constexpr const char* loadNames[]{"LoadLibraryW", "LoadLibraryA", "LoadLibraryExW", "LoadLibraryExA"};
    std::array<FARPROC, 4> loadTargets{};
    std::array<std::array<uint8_t, 16>, 4> before{};
    for (size_t i = 0; i < loadTargets.size(); ++i) {
        loadTargets[i] = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), loadNames[i]);
        Check(loadTargets[i] != nullptr, "Cannot resolve exact loader hook target");
        std::memcpy(before[i].data(), reinterpret_cast<const void*>(loadTargets[i]), before[i].size());
    }
    lan3d::NativeGpuHooks hooks;
    const auto self = GetModuleHandleW(nullptr);
    Check(lan3d::ReadNativeGpuModule(self, hooks.modules[0]), "Cannot identify CPU hook fixture image");
    hooks.moduleCount = 2; hooks.siteCount = 1;
    wcscpy_s(hooks.modules[1].path, MissingModule); hooks.modules[1].imageSize = 4096;
    hooks.sites[0] = {lan3d::NativeGpuHook::Present, 0, static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&UnusedPresent) - reinterpret_cast<uintptr_t>(self)), 0};
    Check(setHooks(&hooks) == ERROR_SUCCESS, "CPU native hook profile rejected");
    lan3d::CaptureConfig config;
    Check(configure(&config) == ERROR_SUCCESS && start(nullptr) == ERROR_SUCCESS, "Capture startup failed without GPU objects");
    for (size_t i = 0; i < loadTargets.size(); ++i)
        Check(std::memcmp(before[i].data(), reinterpret_cast<const void*>(loadTargets[i]), before[i].size()) != 0,
            "Capture did not install an expected LoadLibrary hook");
    const auto mainThread = GetCurrentThreadId();
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        ResetEvent(fixtureEntered); ResetEvent(proceed); ResetEvent(workerQuery); ResetEvent(nestedReturned);
        queryThread = 0; queryCaller = nullptr; queryWaitFailed = false;
        const wchar_t mode[]{static_cast<wchar_t>(L'0' + attempt), 0};
        Check(SetEnvironmentVariableW(L"LAN3D_CAPTURE_LOADER_TEST", mode) != FALSE, "Cannot select loader fixture mode");
        armed = true;
        FixtureLoad load{argv[2]};
        DWORD loaderThread{};
        const auto loader = CreateThread(nullptr, 0, LoadFixture, &load, 0, &loaderThread);
        Check(loader != nullptr, "Cannot create test-only loading thread");
        Check(WaitForSingleObject(fixtureEntered, 5000) == WAIT_OBJECT_0, "Fixture did not enter DllMain");
        Check(WaitForSingleObject(workerQuery, 5000) == WAIT_OBJECT_0, "Worker did not query modules while holding controlMutex");
        Check(queryThread.load() && queryThread.load() != mainThread && queryThread.load() != loaderThread,
            "The blocked query did not come from the capture worker");
        MEMORY_BASIC_INFORMATION memory{};
        Check(VirtualQuery(queryCaller.load(), &memory, sizeof(memory)) == sizeof(memory) && memory.AllocationBase == capture,
            "The blocked module query did not originate in CaptureHooks");
        Check(WaitForSingleObject(nestedReturned, 0) == WAIT_TIMEOUT, "Nested return event signalled before the load");
        // Worker owns the real production controlMutex and cannot release it
        // before this nested API returns. An inline ScanModules must now block;
        // an event-only loader hook returns and allows that worker to continue.
        std::cout << "LOADER_CASE " << loadNames[attempt] << " worker=" << queryThread.load()
            << " loader=" << loaderThread << " target=" << reinterpret_cast<const void*>(loadTargets[attempt]) << '\n' << std::flush;
        SetEvent(proceed);
        Check(WaitForSingleObject(loader, 3000) == WAIT_OBJECT_0, "Nested load deadlocked with capture module scanning");
        armed = false;
        Check(load.loaded && !queryWaitFailed.load(), "Nested loader fixture failed or scanner escaped by timeout");
        Check(WaitForSingleObject(nestedReturned, 0) == WAIT_OBJECT_0, "Fixture did not confirm the real nested API returned");
        CloseHandle(loader);
    }
    SetEnvironmentVariableW(L"LAN3D_CAPTURE_LOADER_TEST", nullptr);
    Check(stop(nullptr) == ERROR_SUCCESS, "Capture stop failed");
    Check(presentCalls == 0, "CPU loader regression unexpectedly executed a GPU hook");
    // The pinned capture worker and query observer retain these test events
    // until process exit. Do not remove a hook while a worker can execute it.
    std::cout << "Capture loader-lock regression passed: nested W/A/ExW/ExA loads, no GPU objects\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n' << std::flush;
    // A deliberately reproduced loader deadlock cannot be unwound through
    // DLL_PROCESS_DETACH. Only terminate this self-contained test process.
    TerminateProcess(GetCurrentProcess(), 1);
    return 1;
}
