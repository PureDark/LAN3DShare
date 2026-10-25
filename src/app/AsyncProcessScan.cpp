#include "AsyncProcessScan.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace lan3d {
AsyncProcessScan::AsyncProcessScan(Scan scan) : scan_(std::move(scan)), worker_([this] { Run(); }) {}
AsyncProcessScan::~AsyncProcessScan() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true; pending_.reset();
    }
    changed_.notify_one();
    // Join only at sender shutdown, never while polling a video frame.
    worker_.join();
}
bool AsyncProcessScan::Request(uint32_t pid) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || stopping_) return false;
    pending_ = RequestInfo{pid, ++generation_};
    completed_.reset();
    changed_.notify_one();
    return true;
}
std::unique_ptr<AsyncProcessScan::Result> AsyncProcessScan::TryTake() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    return lock ? std::move(completed_) : nullptr;
}
void AsyncProcessScan::Run() {
    for (;;) {
        RequestInfo request{};
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = *pending_; pending_.reset();
        }
        const auto start = std::chrono::steady_clock::now();
        auto result = std::make_unique<Result>();
        try { *result = scan_(request.pid); }
        catch (const std::exception& error) { result->error = error.what(); }
        catch (...) { result->error = "Process inspection failed"; }
        result->pid = request.pid;
        result->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            if (request.generation == generation_) completed_ = std::move(result);
        }
    }
}
AsyncProcessScan::Result AsyncProcessScan::Inspect(uint32_t pid) {
    Result result; result.pid = pid;
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!handle) return result;
    ProcessIdentity identity;
    const bool read = ReadProcessIdentity(handle, identity, result.error);
    CloseHandle(handle);
    if (!read) return result;
    auto pinned = OpenValidated(identity, result.error);
    if (!pinned) return result;
    const auto processes = ListProcesses();
    const auto found = std::find_if(processes.begin(), processes.end(), [&](const auto& p) { return p.id == pid; });
    // The held process handle makes a reused PID distinguishable even if the
    // process exits while module snapshots are being enumerated.
    if (found != processes.end() && pinned.Alive()) {
        result.info = *found;
        result.process = std::move(pinned);
    }
    return result;
}
}
