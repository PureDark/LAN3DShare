#include <windows.h>
#include <cstdio>
#include <cstring>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    // Deliberately hold the loader lock in this test-only DLL while the test
    // arranges a concurrent capture module query. No game or GPU is involved.
    wchar_t name[128]{};
    swprintf_s(name, L"Local\\LAN3DCaptureLoader_%lu_entered", GetCurrentProcessId());
    const auto entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    swprintf_s(name, L"Local\\LAN3DCaptureLoader_%lu_continue", GetCurrentProcessId());
    const auto proceed = OpenEventW(SYNCHRONIZE, FALSE, name);
    swprintf_s(name, L"Local\\LAN3DCaptureLoader_%lu_nested_returned", GetCurrentProcessId());
    const auto returned = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!entered || !proceed || !returned) {
        if (entered) CloseHandle(entered);
        if (proceed) CloseHandle(proceed);
        if (returned) CloseHandle(returned);
        return FALSE;
    }
    SetEvent(entered);
    const auto wait = WaitForSingleObject(proceed, 5000);
    CloseHandle(entered); CloseHandle(proceed);
    if (wait != WAIT_OBJECT_0) { CloseHandle(returned); return FALSE; }
    wchar_t wide[MAX_PATH]{}; char ansi[MAX_PATH]{};
    const auto wideLength = GetSystemDirectoryW(wide, MAX_PATH);
    const auto ansiLength = GetSystemDirectoryA(ansi, MAX_PATH);
    if (!wideLength || wideLength + 13 >= MAX_PATH || !ansiLength || ansiLength + 13 >= MAX_PATH) { CloseHandle(returned); return FALSE; }
    wcscat_s(wide, L"\\version.dll"); strcat_s(ansi, "\\version.dll");
    // Resolve the exact kernel32 exports that CaptureHooks detours; an IAT
    // import might use an API-set implementation which bypasses that address.
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto loadW = reinterpret_cast<HMODULE(WINAPI*)(LPCWSTR)>(GetProcAddress(kernel, "LoadLibraryW"));
    const auto loadA = reinterpret_cast<HMODULE(WINAPI*)(LPCSTR)>(GetProcAddress(kernel, "LoadLibraryA"));
    const auto loadExW = reinterpret_cast<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)>(GetProcAddress(kernel, "LoadLibraryExW"));
    const auto loadExA = reinterpret_cast<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)>(GetProcAddress(kernel, "LoadLibraryExA"));
    wchar_t mode[2]{};
    if (!loadW || !loadA || !loadExW || !loadExA ||
        GetEnvironmentVariableW(L"LAN3D_CAPTURE_LOADER_TEST", mode, 2) != 1) { CloseHandle(returned); return FALSE; }
    HMODULE module{};
    switch (mode[0]) {
    case L'0': module = loadW(wide); break;
    case L'1': module = loadA(ansi); break;
    case L'2': module = loadExW(wide, nullptr, 0); break;
    case L'3': module = loadExA(ansi, nullptr, 0); break;
    }
    // Only the real API returning may unblock the scanner. The observer never
    // waits in a loader detour or replaces the production callback's behavior.
    if (module) SetEvent(returned);
    CloseHandle(returned);
    if (!module) return FALSE;
    FreeLibrary(module);
    return TRUE;
}
