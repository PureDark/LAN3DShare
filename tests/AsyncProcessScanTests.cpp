#include "app/AsyncProcessScan.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void Request(lan3d::AsyncProcessScan& scan, uint32_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!scan.Request(pid)) {
        Check(std::chrono::steady_clock::now() < deadline, "request mailbox blocked");
        std::this_thread::yield();
    }
}
static auto Take(lan3d::AsyncProcessScan& scan) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    for (;;) {
        if (auto result = scan.TryTake()) return result;
        Check(std::chrono::steady_clock::now() < deadline, "missing completed scan");
        std::this_thread::sleep_for(1ms);
    }
}
int main() try {
    std::atomic<bool> entered{}, release{}, timedOut{};
    std::vector<uint32_t> calls;
    {
        lan3d::AsyncProcessScan scan([&](uint32_t pid) {
            calls.push_back(pid);
            if (pid == 1) {
                entered = true;
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (!release && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
                timedOut = !release;
            }
            if (pid == 4) throw std::runtime_error("test scan failure");
            lan3d::AsyncProcessScan::Result result;
            result.info.id = pid;
            return result;
        });
        Request(scan, 1);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        Check(entered, "worker did not start");
        // The scanner cannot finish until this thread releases it. Polling and
        // replacing requests must remain usable while the callback is blocked.
        for (int i = 0; i < 1000; ++i) Check(!scan.TryTake(), "premature result");
        Request(scan, 2);
        Request(scan, 3);
        release = true;
        auto latest = Take(scan);
        Check(!timedOut && latest->pid == 3 && latest->info.id == 3, "stale scan accepted or caller blocked");
        Request(scan, 4);
        Check(Take(scan)->error == "test scan failure", "worker exception not reported");
        Request(scan, 5);
        Check(Take(scan)->info.id == 5, "worker did not recover");
    }
    Check(calls == std::vector<uint32_t>({1, 3, 4, 5}), "pending requests were not coalesced");
    // Exercise the real inspector and process-identity handoff without hooks.
    {
        lan3d::AsyncProcessScan scan;
        Request(scan, GetCurrentProcessId());
        const auto result = Take(scan);
        Check(result->pid == GetCurrentProcessId(), "real inspector lost requested PID");
        // ListProcesses intentionally excludes this process.
        Check(!result->process && !result->info.leiaLoaded, "self selected as source");
    }
    std::cout << "Async process scan: nonblocking poll, coalescing, stale result rejection, exception recovery, shutdown and real inspection passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; return 1;
}
