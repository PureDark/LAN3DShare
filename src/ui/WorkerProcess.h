#pragma once
#include <Windows.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lan3d::ui {
enum class WorkerMode { Sender, Receiver };
struct WorkerProcessOptions {
    DWORD stopWarningAfterMs{35000};
    DWORD destructorWaitMs{5000}; // Clamped to five seconds; never force-kills.
};

// UI-thread-owned process helper. Start uses CreateProcessW directly, an explicit
// executable path, correctly quoted argv and an explicit inherited-handle list.
// Poll/RequestStop never wait for process exit or terminate the source game.
class WorkerProcess {
public:
    explicit WorkerProcess(WorkerProcessOptions options = {});
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    bool Start(const std::filesystem::path& executable, WorkerMode mode,
               const std::vector<std::wstring>& arguments, std::wstring& error,
               const std::filesystem::path& logDirectory = {});
    void RequestStop();
    void Poll();
    bool Ready() const;
    bool Running() const;
    bool StopRequested() const;
    bool StopOverdue() const;
    DWORD ProcessId() const;
    std::optional<DWORD> ExitCode() const;
    std::wstring TailLog(size_t maxCharacters = 6000) const;
    std::filesystem::path LogPath() const;
    std::wstring Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
