#pragma once
#include "audio/AudioScheduler.h"
#include "audio/AudioMeter.h"
#include <memory>
#include <string>

namespace lan3d::audio {
struct PlaybackStats : SyncStats {
    bool ready{};
    Level receivedLevel, outputLevel;
    uint64_t submittedFrames{};
    uint64_t repairedSamples{}, limitedFrames{}, underruns{}, protectionRestarts{};
    uint32_t paddingFrames{}, maxPaddingFrames{};
    unsigned volumePercent{100}, appliedVolumePercent{100};
};
class Playback {
public:
    Playback();
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    void Start();
    void Stop();
    void Reset(uint64_t generation);
    void Push(const Block& block, uint64_t generation);
    void Presented(uint64_t videoPts, uint64_t generation);
    void Suspend();
    void SetVolume(unsigned percent);
    std::string Status() const;
    PlaybackStats Stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
