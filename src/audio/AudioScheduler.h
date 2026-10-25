#pragma once
#include "audio/AudioTimeline.h"

namespace lan3d::audio {
struct SyncStats {
    bool suspended{true};
    uint64_t generation{}, acceptedFrames{}, releasedFrames{}, droppedFrames{}, staleBlocks{};
    uint64_t resets{}, stalls{}, reanchors{}, maxAhead100ns{};
    uint32_t queuedFrames{}, maxQueueFrames{};
};
enum ResetReason : uint32_t { SessionReset = 1, SourceDiscontinuity = 2, VideoRollback = 4,
    VideoJump = 8, WindowSuspended = 16, EndpointChanged = 32, VideoStalled = 64, AudioLag = 128 };
struct RenderPlan { bool resetEndpoint{}; Block audio; uint32_t resetReasons{}; };

// Thread-independent scheduling policy. The only presentation credit comes from
// a successfully displayed, advancing video timestamp. Tests supply a fake local
// monotonic clock; WASAPI uses the same policy with real padding and QPC.
class PlaybackScheduler {
public:
    static constexpr uint32_t MaxQueuedFrames = SampleRate / 10;
    static constexpr uint32_t MaxEndpointFrames = SampleRate / 50; // 20 ms
    static constexpr uint64_t MaxLag100ns = 800000;                // 80 ms bounded recovery window
    static constexpr uint64_t Prebuffer100ns = 400000;             // 40 ms of already-presented audio
    static constexpr uint64_t Stall100ns = 500000;                 // 50 ms
    void Reset(uint64_t generation);
    void Push(const Block& block, uint64_t generation);
    void Presented(uint64_t pts, uint64_t generation, uint64_t now100ns);
    void Suspend();
    void EndpointChanged();
    void Rebuffer() { prebuffering_ = true; prebufferSince_ = 0; }
    RenderPlan Plan(uint32_t paddingFrames, uint32_t availableFrames, uint64_t now100ns, bool deferResetAudio = false);
    SyncStats Stats() const;
private:
    void Flush(bool clearQueue, uint32_t reason);
    TimelineQueue queue_;
    SyncStats stats_;
    uint64_t videoPts_{}, videoWall_{}, lastQueuedEnd_{}, releasedEnd_{}, prebufferSince_{};
    bool suspended_{true}, resetPending_{true}, stalled_{};
    bool prebuffering_{true};
    uint32_t resetReasons_{};
};
}
