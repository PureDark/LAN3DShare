#include "audio/AudioScheduler.h"

namespace lan3d::audio {
void PlaybackScheduler::Flush(bool clearQueue, uint32_t reason) {
    if (clearQueue) stats_.droppedFrames += queue_.Clear();
    releasedEnd_ = 0; resetPending_ = true; Rebuffer(); resetReasons_ |= reason; ++stats_.resets;
}
void PlaybackScheduler::Reset(uint64_t generation) {
    Flush(true, SessionReset); stats_.generation = generation;
    videoPts_ = videoWall_ = lastQueuedEnd_ = 0;
    suspended_ = true; stalled_ = false;
}
void PlaybackScheduler::Push(const Block& block, uint64_t generation) {
    if (generation != stats_.generation) { ++stats_.staleBlocks; return; }
    if (!ValidBlock(block) || block.samples.empty()) return;
    // A discontinuity or genuine source-clock rollback invalidates audio already
    // queued at the endpoint, even before the corresponding picture is shown.
    if (block.discontinuity || (lastQueuedEnd_ > block.End100ns() && lastQueuedEnd_ - block.End100ns() > 20000)) {
        Flush(true, SourceDiscontinuity); ++stats_.reanchors;
    }
    lastQueuedEnd_ = block.End100ns(); stats_.acceptedFrames += block.Frames();
    stats_.droppedFrames += queue_.Push(block, MaxQueuedFrames);
    if (videoPts_ > MaxLag100ns) stats_.droppedFrames += queue_.TrimBefore(videoPts_ - MaxLag100ns);
    stats_.maxQueueFrames = std::max(stats_.maxQueueFrames, queue_.Frames());
}
void PlaybackScheduler::Presented(uint64_t pts, uint64_t generation, uint64_t now100ns) {
    if (generation != stats_.generation || !pts) return;
    // A UI redraw of a stale texture cannot renew audio permission.
    if (pts == videoPts_) return;
    if (videoPts_ && pts < videoPts_) {
        Flush(true, VideoRollback); ++stats_.reanchors;
        videoPts_ = pts; videoWall_ = now100ns;
        suspended_ = true; stalled_ = false; return;
    }
    if (videoPts_ && pts - videoPts_ > Stall100ns) { Flush(false, VideoJump); ++stats_.reanchors; }
    videoPts_ = pts; videoWall_ = now100ns;
    suspended_ = false; stalled_ = false;
    if (pts > MaxLag100ns) stats_.droppedFrames += queue_.TrimBefore(pts - MaxLag100ns);
}
void PlaybackScheduler::Suspend() {
    if (suspended_) return;
    Flush(true, WindowSuspended); suspended_ = true; stalled_ = false;
}
void PlaybackScheduler::EndpointChanged() { Flush(false, audio::EndpointChanged); }
RenderPlan PlaybackScheduler::Plan(uint32_t paddingFrames, uint32_t availableFrames, uint64_t now100ns, bool deferResetAudio) {
    if (!suspended_ && videoWall_ && now100ns > videoWall_ && now100ns - videoWall_ > Stall100ns) {
        Flush(true, VideoStalled); suspended_ = true; stalled_ = true; ++stats_.stalls;
    }
    // Padding represents samples already submitted to this endpoint. If the
    // displayed timeline jumped beyond them, stop/reset before choosing new audio.
    if (!suspended_ && releasedEnd_ && videoPts_ > releasedEnd_ && videoPts_ - releasedEnd_ > MaxLag100ns) {
        Flush(false, AudioLag); ++stats_.reanchors;
    }
    RenderPlan plan; plan.resetEndpoint = resetPending_; resetPending_ = false;
    plan.resetReasons = resetReasons_; resetReasons_ = 0;
    // The device may need to finish a short fade before it can accept new audio.
    if (plan.resetEndpoint && deferResetAudio) return plan;
    if (suspended_ || !videoPts_) return plan;
    if (plan.resetEndpoint) paddingFrames = 0;
    if (prebuffering_) {
        const auto begin = queue_.Begin(), end = std::min(videoPts_, queue_.End());
        if (!begin || end <= begin) return plan;
        if (!prebufferSince_) prebufferSince_ = now100ns;
        // A short sound followed by capture silence must not wait forever.
        if (end - begin < Prebuffer100ns && now100ns - prebufferSince_ < Prebuffer100ns) return plan;
        prebuffering_ = false;
    }
    const auto budget = paddingFrames < MaxEndpointFrames ? MaxEndpointFrames - paddingFrames : 0;
    plan.audio = queue_.TakeThrough(videoPts_, std::min(availableFrames, budget));
    if (!plan.audio.samples.empty()) {
        releasedEnd_ = plan.audio.End100ns(); stats_.releasedFrames += plan.audio.Frames();
        if (releasedEnd_ > videoPts_) stats_.maxAhead100ns = std::max(stats_.maxAhead100ns, releasedEnd_ - videoPts_);
    }
    return plan;
}
SyncStats PlaybackScheduler::Stats() const { auto out = stats_; out.suspended = suspended_; out.queuedFrames = queue_.Frames(); return out; }
}
