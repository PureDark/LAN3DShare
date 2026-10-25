#pragma once
#include "AudioMeter.h"
#include <functional>
#include <memory>

namespace lan3d::audio {
struct Telemetry {
    uint32_t receiver{}, enabled{}, ready{}, suspended{}, volume{100}, clockCorrected{};
    Level input, output;
    uint64_t inputFrames{}, outputFrames{}, droppedFrames{}, timestampErrors{};
    char detail[192]{};
};
// Independent 10 Hz observer. Never performs UI/IPC from the audio callback.
class TelemetryPublisher {
public:
    explicit TelemetryPublisher(std::function<Telemetry()> snapshot);
    ~TelemetryPublisher();
    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool ReadTelemetry(uint32_t pid, Telemetry& result) noexcept;
}
