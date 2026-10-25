#include "common/FrameBus.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) try {
    std::string error;
    if (argc == 3 && std::wstring(argv[1]) == L"--child") {
        lan3d::FrameBus reader; lan3d::Frame frame;
        Check(reader.Open(std::stoul(argv[2]), false, error), error.c_str());
        Check(reader.ReadLatest(frame, 0), "child frame unavailable");
        Check(frame.width == 64 && frame.height == 32 && frame.stride == 256 && frame.sequence == 42, "child metadata mismatch");
        for (size_t i = 0; i < frame.bgra.size(); ++i) Check(frame.bgra[i] == uint8_t(i % 251), "cross-process payload mismatch");
        Check(reader.Status() == "cross-process test", "status mismatch");
        return 0;
    }
    lan3d::FrameBus writer, reader;
    Check(writer.Open(GetCurrentProcessId(), true, error), error.c_str());
    Check(reader.Open(GetCurrentProcessId(), false, error), error.c_str());
    lan3d::Frame f{64, 32, 256, 42, 123456, {}};
    f.bgra.resize(size_t(f.stride) * f.height);
    for (size_t i = 0; i < f.bgra.size(); ++i) f.bgra[i] = uint8_t(i % 251);
    Check(writer.Publish(f), "publish failed");
    writer.SetStatus("cross-process test");
    lan3d::Frame out;
    Check(reader.ReadLatest(out, 0) && out.bgra == f.bgra, "same-process read mismatch");
    Check(!reader.ReadLatest(out, 42), "duplicate frame returned");
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, _countof(exe));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    Check(CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi), "create reader process failed");
    const auto wait = WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    Check(wait == WAIT_OBJECT_0 && code == 0, "child bus verification failed");
    const auto lockName = L"Local\\LAN3DShare.v2." + std::to_wstring(GetCurrentProcessId()) + L".lock";
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
    Check(mutex != nullptr, "open test mutex failed");
    Check(WaitForSingleObject(mutex, 1000) == WAIT_OBJECT_0, "lock failed");
    bool published = true;
    std::thread contender([&] { published = writer.Publish(f); });
    contender.join(); ReleaseMutex(mutex); CloseHandle(mutex);
    Check(!published, "render producer waited or overwrote a locked slot");
    f.width = 63; Check(!writer.Publish(f), "odd SBS width accepted"); f.width = 64;
    f.bgra.resize(10); Check(!writer.Publish(f), "truncated frame accepted");
    writer.Close(); writer.Close();
    Check(!writer.Publish(f) && writer.Status().empty(), "closed writer retained its mapping");
    writer.SetStatus("must not affect remaining reader");
    Check(reader.ReadLatest(out, 0) && out.sequence == 42 && reader.Status() == "cross-process test", "writer close changed another participant's mapping");
    reader.Close();
    Check(!reader.ReadLatest(out, 0) && reader.Status().empty(), "closed reader retained its mapping");
    Check(!reader.Open(GetCurrentProcessId(), false, error), "old PID mapping survived all participants closing");
    Check(writer.Open(GetCurrentProcessId(), true, error), error.c_str());
    Check(reader.Open(GetCurrentProcessId(), false, error), error.c_str());
    Check(reader.Status().empty() && !reader.ReadLatest(out, 0), "new generation inherited an old frame or status");
    f.sequence = 1; f.bgra.resize(size_t(f.stride) * f.height, 23);
    Check(writer.Publish(f), "new generation publish failed");
    Check(reader.ReadLatest(out, 0) && out.sequence == 1 && out.bgra == f.bgra, "new generation did not restart at sequence one");
    std::cout << "FrameBus: cross-process bytes, monotonic read, contention, invalid frames and closed-generation reset passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
