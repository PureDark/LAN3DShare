#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>

namespace lan3d {
class WorkerEvent {
public:
    WorkerEvent(const std::wstring& name, DWORD access) {
        if (name.empty()) return;
        handle_ = OpenEventW(access, FALSE, name.c_str());
        if (!handle_) throw std::runtime_error("Cannot open the desktop worker event (Windows error " + std::to_string(GetLastError()) + ")");
    }
    ~WorkerEvent() { if (handle_) CloseHandle(handle_); }
    WorkerEvent(const WorkerEvent&) = delete;
    WorkerEvent& operator=(const WorkerEvent&) = delete;
    bool IsSet() const { return handle_ && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0; }
    void Signal() const {
        if (handle_ && !SetEvent(handle_)) throw std::runtime_error("Cannot signal desktop sender readiness");
    }
private:
    HANDLE handle_{};
};
}
