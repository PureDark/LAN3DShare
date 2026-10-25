#pragma once
#include "Frame.h"
#include <memory>
#include <string>

namespace lan3d {
class FrameBus {
public:
    FrameBus();
    ~FrameBus();
    FrameBus(const FrameBus&) = delete;
    FrameBus& operator=(const FrameBus&) = delete;
    bool Open(uint32_t processId, bool create, std::string& error);
    // Release only this instance's mapping and handles. The object can reopen.
    void Close();
    // Never wait in the game's render thread. Dropping a capture is allowed.
    bool Publish(const Frame& frame);
    bool ReadLatest(Frame& frame, uint64_t afterSequence);
    void SetStatus(const std::string& text);
    std::string Status();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
uint64_t Timestamp100ns();
}
