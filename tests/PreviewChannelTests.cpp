#include "ui/PreviewChannel.h"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void CheckImage(const lan3d::ui::PreviewImage& image) {
    const auto& frame = image.frame;
    Check(image.sourceWidth == 3840 && image.sourceHeight == 2160, "Original dimensions were lost");
    Check(frame.width == 432 && frame.height == 242 && frame.stride == 1728 && frame.sequence == 42 &&
        frame.timestamp100ns == 123456 && frame.kind == lan3d::ContentKind::LeiaSbs && lan3d::ValidFrame(frame), "Thumbnail metadata mismatch");
    for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
        const auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + x * 4;
        const bool right = x >= frame.width / 2;
        Check(pixel[0] == (right ? 200 : 12) && pixel[1] == (right ? 170 : 23) &&
            pixel[2] == (right ? 140 : 34) && pixel[3] == 255, "SBS eye pixels or row pitch were corrupted");
    }
}
}
int wmain(int argc, wchar_t** argv) try {
    if (argc == 3 && std::wstring(argv[1]) == L"--child") {
        lan3d::ui::PreviewImage image;
        Check(lan3d::ui::ReadPreview(std::stoul(argv[2]), image), "Cross-process preview unavailable");
        CheckImage(image); return 0;
    }
    const auto pid = GetCurrentProcessId();
    lan3d::ui::PreviewImage image;
    Check(!lan3d::ui::ReadPreview(pid, image), "Unpublished preview was readable");
    Check(!lan3d::ui::ReadPreview(0, image), "Zero process ID accepted");
    {
        lan3d::ui::PreviewPublisher publisher;
        lan3d::Frame frame;
        publisher.Publish(frame);
        Check(!lan3d::ui::ReadPreview(pid, image), "Invalid frame created a preview");
        frame.width = 3840; frame.height = 2160; frame.stride = frame.width * 4 + 16;
        frame.sequence = 42; frame.timestamp100ns = 123456;
        frame.bgra.resize(size_t(frame.stride) * frame.height, 99);
        for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
            auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + x * 4;
            const bool right = x >= frame.width / 2;
            pixel[0] = right ? 200 : 12; pixel[1] = right ? 170 : 23; pixel[2] = right ? 140 : 34; pixel[3] = 255;
        }
        publisher.Publish(frame);
        Check(lan3d::ui::ReadPreview(pid, image), "First valid preview unavailable"); CheckImage(image);
        frame.sequence = 43; frame.kind = lan3d::ContentKind::Desktop; publisher.Publish(frame);
        Check(lan3d::ui::ReadPreview(pid, image) && image.frame.sequence == 42, "Preview publishing is not limited to one Hz");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, _countof(executable));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child " + std::to_wstring(pid);
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child), "Cannot create cross-process reader");
        const auto wait = WaitForSingleObject(child.hProcess, 5000);
        DWORD exitCode{1}; GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        Check(wait == WAIT_OBJECT_0 && exitCode == 0, "Cross-process preview verification failed");
        const auto name = L"Local\\LAN3DShare.preview.v1." + std::to_wstring(pid);
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".lock").c_str());
        Check(mutex && WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0, "Cannot lock preview for contention test");
        bool read{true};
        std::thread contender([&] { lan3d::ui::PreviewImage blocked; read = lan3d::ui::ReadPreview(pid, blocked); });
        contender.join();
        Check(!read, "Preview reader blocked or read a busy slot");
        Sleep(1050);
        std::thread busyPublisher([&] { publisher.Publish(frame); });
        busyPublisher.join();
        ReleaseMutex(mutex); CloseHandle(mutex);
        Check(lan3d::ui::ReadPreview(pid, image) && image.frame.sequence == 42, "Preview publisher overwrote a busy slot");
        Sleep(1050); publisher.Publish(frame);
        Check(lan3d::ui::ReadPreview(pid, image) && image.frame.sequence == 43 &&
            image.frame.kind == lan3d::ContentKind::Desktop, "Next preview/content-kind update failed");
        HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name.c_str());
        Check(mapping != nullptr, "Cannot open preview corruption probe");
        auto* words = static_cast<uint32_t*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 64));
        Check(words != nullptr, "Cannot map preview corruption probe");
        words[7] = 0xffffffffu; // Invalid pixel-byte length must not drive an allocation or read.
        Check(!lan3d::ui::ReadPreview(pid, image) && image.frame.sequence == 43, "Malformed length was read or changed output");
        words[7] = image.frame.stride * image.frame.height;
        words[4] = 8192; // Oversized thumbnail dimensions are also untrusted.
        Check(!lan3d::ui::ReadPreview(pid, image), "Oversized thumbnail accepted");
        UnmapViewOfFile(words); CloseHandle(mapping);
    }
    Check(!lan3d::ui::ReadPreview(pid, image), "Preview survived its worker/publisher");
    std::cout << "PreviewChannel PASS: real 4K-to-thumbnail SBS pixels, bounded metadata, cross-process read, one-Hz publishing, contention, invalid input, cleanup\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
