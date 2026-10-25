#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lan3d {
struct ProcessIdentity {
    uint32_t pid{};
    uint64_t creationTime{};
    std::wstring canonicalPath;
};

// A PID alone can identify a different process after an exit. Read and validate
// identity on the same handle that will be used for subsequent operations.
bool ReadProcessIdentity(HANDLE process, ProcessIdentity& identity, std::string& error);
bool CheckExpectedProcess(HANDLE process, const ProcessIdentity& expected, std::string& error);

// Exact executable path only; excludes this process. Empty with no error means
// no live instance. More than one match is deliberately left ambiguous.
std::vector<ProcessIdentity> FindMatchingProcesses(const std::wstring& executable, std::string& error);

class PinnedProcess {
public:
    PinnedProcess() = default;
    ~PinnedProcess();
    PinnedProcess(PinnedProcess&& other) noexcept;
    PinnedProcess& operator=(PinnedProcess&& other) noexcept;
    PinnedProcess(const PinnedProcess&) = delete;
    PinnedProcess& operator=(const PinnedProcess&) = delete;
    explicit operator bool() const { return handle_ != nullptr; }
    bool Alive() const;
    const ProcessIdentity& identity() const { return identity_; }
    HANDLE handle() const { return handle_; }
private:
    friend PinnedProcess OpenValidated(const ProcessIdentity&, std::string&);
    HANDLE handle_{};
    ProcessIdentity identity_;
};

PinnedProcess OpenValidated(const ProcessIdentity& expected, std::string& error);
}
