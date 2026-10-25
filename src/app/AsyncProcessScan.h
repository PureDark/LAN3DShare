#pragma once
#include "Injector.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace lan3d {
// One worker, one pending request and one completed result. Enumeration and
// identity validation never run under the mailbox lock or on the sender.
class AsyncProcessScan {
public:
    struct Result {
        uint32_t pid{};
        ProcessInfo info;
        PinnedProcess process;
        std::string error;
        double milliseconds{};
    };
    using Scan = std::function<Result(uint32_t)>;
    explicit AsyncProcessScan(Scan scan = Inspect);
    ~AsyncProcessScan();
    AsyncProcessScan(const AsyncProcessScan&) = delete;
    AsyncProcessScan& operator=(const AsyncProcessScan&) = delete;
    // Both calls use try_lock. A busy mailbox is retried on a later frame.
    bool Request(uint32_t pid);
    std::unique_ptr<Result> TryTake();
private:
    static Result Inspect(uint32_t pid);
    void Run();
    struct RequestInfo { uint32_t pid; uint64_t generation; };
    Scan scan_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<RequestInfo> pending_;
    std::unique_ptr<Result> completed_;
    uint64_t generation_{};
    bool stopping_{};
    std::thread worker_;
};
}
