#pragma once
#include "GpuFrame.h"
#include <memory>
#include <string>

namespace lan3d {
// Bounded latest-frame mailbox. All operations are non-blocking; saturation
// drops new work. A held lease is never expired on a timer or stolen.
class GpuFrameBus {
public:
    GpuFrameBus();
    ~GpuFrameBus();
    GpuFrameBus(const GpuFrameBus&) = delete;
    bool Open(uint32_t processId, bool create, std::string& error);
    // Release this instance's handles without acknowledging any Held lease.
    // Other participants keep their mapping and quarantine those reads.
    void Close();
    bool Publish(const GpuFrame& frame);
    bool IsReleased(uint64_t sequence);
    bool AcquireLatest(GpuFrame& frame, uint64_t afterSequence);
    // Call only after the consumer GPU has finished reading the input.
    bool Release(uint64_t sequence);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
