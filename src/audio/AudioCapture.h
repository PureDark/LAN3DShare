#pragma once
#include "audio/AudioMeter.h"
#include <memory>
#include <string>

namespace lan3d::audio {
struct CaptureStats {
    bool ready{}, clockCorrected{};
    Level level;
    uint64_t rawTimestamp100ns{}, clockCorrections{};
    uint64_t capturedFrames{}, packets{}, discardedFrames{}, timestampErrors{}, latestTimestamp100ns{};
    uint64_t latestRead100ns{}, streamLatency100ns{}, maxCaptureLead100ns{};
    uint32_t queuedFrames{}, endpointBufferFrames{};
};
class LoopbackCapture {
public:
    LoopbackCapture();
    ~LoopbackCapture();
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    void Start();
    void Stop();
    Block TakeThrough(uint64_t videoPts);
    std::string Status() const;
    CaptureStats Stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
