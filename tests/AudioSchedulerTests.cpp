#include "audio/AudioScheduler.h"
#include <iostream>
#include <stdexcept>

using namespace lan3d::audio;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Block Silence(uint64_t pts, uint32_t frames, bool discontinuity = false) {
    Block block; block.timestamp100ns = pts; block.samples.resize(size_t(frames) * Channels);
    block.discontinuity = discontinuity; return block;
}
void NormalAndStall() {
    PlaybackScheduler scheduler; scheduler.Reset(1);
    constexpr uint64_t origin = 100000000, wallOrigin = 1000000;
    uint32_t padding{}; uint64_t lastVideo{}, released{};
    for (uint64_t now = 0; now <= 10000000; now += 20000) {
        padding = padding > 96 ? padding - 96 : 0;
        if (now && now % 100000 == 0) scheduler.Push(Silence(origin + now - 100000, 480), 1);
        const auto video = origin + (now / 166666) * 166666;
        if (video != lastVideo) { scheduler.Presented(video, 1, wallOrigin + now); lastVideo = video; }
        auto plan = scheduler.Plan(padding, 960 - padding, wallOrigin + now);
        if (plan.resetEndpoint) padding = 0;
        Require(plan.audio.samples.empty() || plan.audio.End100ns() <= lastVideo, "audio went beyond presented picture");
        padding += plan.audio.Frames(); released += plan.audio.Frames();
        Require(padding <= 960 && scheduler.Stats().queuedFrames <= 4800, "unbounded playback queue");
    }
    Require(released > 45000, "normal 60 FPS playback lost excessive audio");
    const auto permitted = lastVideo;
    for (uint64_t now = 10020000; now <= 12500000; now += 20000) {
        padding = padding > 96 ? padding - 96 : 0;
        if (now % 100000 == 0) scheduler.Push(Silence(origin + now - 100000, 480), 1);
        // A frozen texture may still be rendered every refresh; that is not new credit.
        scheduler.Presented(lastVideo, 1, wallOrigin + now);
        auto plan = scheduler.Plan(padding, 960 - padding, wallOrigin + now);
        if (plan.resetEndpoint) padding = 0;
        Require(plan.audio.samples.empty() || plan.audio.End100ns() <= permitted, "stalled video allowed later sound");
        if (now > 10600000) Require(plan.audio.samples.empty(), "audio continued after video freeze");
        padding += plan.audio.Frames();
    }
    Require(scheduler.Stats().stalls == 1, "stale redraw prolonged audio or repeatedly reset endpoint");
    scheduler.Presented(origin + 12500000, 1, wallOrigin + 12500000);
    auto resumed = scheduler.Plan(960, 960, wallOrigin + 12500000);
    Require(resumed.resetEndpoint, "video jump retained old endpoint audio");
    Require(!resumed.audio.samples.empty(), "video jump failed to resume sound");
    Require(resumed.audio.timestamp100ns >= origin + 12500000 - PlaybackScheduler::MaxLag100ns,
        "video jump replayed old audio");
    Require(scheduler.Stats().maxAhead100ns == 0, "audio submission exceeded video PTS");
}
void GenerationsAndSuspend() {
    PlaybackScheduler scheduler; scheduler.Reset(7);
    scheduler.Push(Silence(10000000, 4800), 7); scheduler.Presented(11000000, 7, 1000000);
    Require(!scheduler.Plan(0, 960, 1000000).audio.samples.empty(), "initial playback failed");
    scheduler.Reset(8);
    scheduler.Push(Silence(11000000, 480), 7); scheduler.Presented(11100000, 7, 1100000);
    auto reset = scheduler.Plan(960, 960, 1100000);
    Require(reset.resetEndpoint && reset.audio.samples.empty(), "old connection survived reset");
    Require(scheduler.Stats().staleBlocks == 1, "stale generation was not counted");
    scheduler.Push(Silence(20000000, 1920), 8); scheduler.Presented(20400000, 8, 1200000);
    Require(scheduler.Plan(0, 960, 1200000).audio.Frames() == 960, "new generation did not resume");
    scheduler.Suspend(); scheduler.Suspend();
    scheduler.Push(Silence(20400000, 1920), 8);
    scheduler.Presented(20400000, 8, 1300000);
    Require(scheduler.Plan(960, 960, 1300000).audio.samples.empty(), "suspend resumed on stale picture");
    scheduler.Presented(20800000, 8, 1400000);
    Require(!scheduler.Plan(0, 960, 1400000).audio.samples.empty(), "fresh picture failed to leave suspend");
}
void RollbackAndBounds() {
    PlaybackScheduler scheduler; scheduler.Reset(1);
    for (uint64_t i = 0; i < 100; ++i) scheduler.Push(Silence(100000000 + i * 1000000, 4800), 1);
    Require(scheduler.Stats().queuedFrames <= 4800 && scheduler.Stats().droppedFrames > 470000,
        "queued audio grew during a long stall");
    scheduler.Presented(200000000, 1, 1000000); scheduler.Plan(0, 960, 1000000);
    scheduler.Presented(100000000, 1, 1100000);
    auto rollback = scheduler.Plan(960, 960, 1100000);
    Require(rollback.resetEndpoint && rollback.audio.samples.empty(), "clock rollback replayed stale audio");
    scheduler.Push(Silence(100000000, 1920), 1); scheduler.Presented(100400000, 1, 1200000);
    auto fresh = scheduler.Plan(0, 960, 1200000);
    Require(!fresh.audio.samples.empty() && fresh.audio.End100ns() <= 100400000, "clock rollback failed to reanchor");
    scheduler.Push(Silence(100400000, 1920, true), 1); scheduler.Presented(100800000, 1, 1300000);
    Require(scheduler.Plan(960, 960, 1300000).resetEndpoint, "capture discontinuity retained endpoint queue");
}
void TimestampBoundary() {
    TimelineQueue queue;
    queue.Push(Silence(10000000, 480), 7200);
    queue.Push(Silence(10100100, 480), 7200); // 10 us QPC jitter
    auto block = queue.TakeThrough(10150000, 960);
    Require(block.End100ns() <= 10150000, "concatenated packet crossed picture timestamp");
    Require(block.Frames() > 700, "small capture timestamp jitter prevented combining packets");
    queue.Clear(); queue.Push(Silence(10000000, 480), 7200); queue.Push(Silence(10300000, 480), 7200);
    block = queue.TakeThrough(10400000, 960);
    Require(block.Frames() == 480, "capture gap was collapsed into contiguous sound");
    queue.Clear(); queue.Push(Silence(10000000, 4800), 7200);
    Require(queue.TrimBefore(10900000) == 4320 && queue.Frames() == 480, "partial timestamp trim failed");
    queue.Clear(); queue.Push(Silence(10000000, 800), 7200);
    uint64_t samples{};
    for (unsigned i = 0; i < 800; ++i) samples += queue.TakeThrough(10166666, 1).Frames();
    Require(samples == 800 && queue.Frames() == 0, "fractional sample boundaries lost or duplicated audio");
    Require(FramesThrough(10000000, 10166666) == 800, "fractional frame count was rounded down twice");
    queue.Push(Silence(10200000, 480), 7200);
    Require(queue.TakeThrough(10100000, 960).samples.empty() && queue.Frames() == 480,
        "future loopback samples were released early");
    Require(queue.TakeThrough(10300000, 960).Frames() == 480, "future loopback samples were not retained");
}
void LongContinuity() {
    PlaybackScheduler scheduler; scheduler.Reset(1);
    constexpr uint64_t origin = 100000000;
    uint32_t padding{}; uint64_t lastVideo{}, released{};
    for (uint64_t now = 0; now <= 100000000; now += 50000) {
        padding = padding > 240 ? padding - 240 : 0;
        if (now && now % 100000 == 0) scheduler.Push(Silence(origin + now - 100000, 480), 1);
        const auto video = origin + (now * 60 / UnitsPerSecond) * UnitsPerSecond / 60;
        if (video != lastVideo) { scheduler.Presented(video, 1, origin + now); lastVideo = video; }
        auto plan = scheduler.Plan(padding, 960 - padding, origin + now);
        if (plan.resetEndpoint) padding = 0;
        padding += plan.audio.Frames(); released += plan.audio.Frames();
        Require(plan.audio.samples.empty() || plan.audio.End100ns() <= video, "continuous sound crossed presented PTS");
    }
    const auto stats = scheduler.Stats();
    Require(stats.acceptedFrames == 480000 && released >= 477600 && released <= 480000,
        "10 second continuity lost or duplicated more than the allowed startup tail");
    Require(stats.resets == 1 && stats.stalls == 0 && stats.reanchors == 0,
        "steady 60 FPS caused repeated endpoint resets");
}
void JitterAndDeferredReset() {
    PlaybackScheduler scheduler; scheduler.Reset(1);
    constexpr uint64_t origin = 100000000;
    uint32_t padding{}; bool playing{}; unsigned dry{};
    for (uint64_t now = 0; now <= 50000000; now += 10000) {
        if (playing && padding < 48) ++dry;
        padding = padding > 48 ? padding - 48 : 0;
        if (now && now % 100000 == 0) scheduler.Push(Silence(origin + now - 100000, 480), 1);
        // A 20 ms decode/presentation hitch once per second, with a fresh
        // texture after it. It must not cause a hard reset or a dry endpoint.
        if (now % UnitsPerSecond < 5000000 || now % UnitsPerSecond >= 5200000)
            scheduler.Presented(origin + (now * 60 / UnitsPerSecond) * UnitsPerSecond / 60, 1, origin + now);
        auto plan = scheduler.Plan(padding, 960 - padding, origin + now, true);
        if (plan.resetEndpoint) padding = 0;
        padding += plan.audio.Frames(); playing |= !plan.audio.samples.empty();
        Require(plan.audio.samples.empty() || plan.audio.End100ns() <= origin + now, "jitter played future audio");
    }
    std::cout << "jitter dry=" << dry << " resets=" << scheduler.Stats().resets << '\n';
    Require(dry == 0 && scheduler.Stats().resets == 1, "short video hitches interrupted buffered audio");
    scheduler.Push(Silence(origin + 50000000, 1920, true), 1);
    scheduler.Presented(origin + 50400000, 1, origin + 50400000);
    const auto before = scheduler.Stats().queuedFrames;
    const auto reset = scheduler.Plan(480, 480, origin + 50400000, true);
    Require(reset.resetEndpoint && (reset.resetReasons & SourceDiscontinuity) && reset.audio.samples.empty() &&
        scheduler.Stats().queuedFrames == before, "fade/drain consumed new source samples prematurely");
    Require(!scheduler.Plan(0, 960, origin + 50500000, true).audio.samples.empty(), "fade completion failed to resume");
    scheduler.Reset(2);
    scheduler.Push(Silence(origin, 240), 2); // A 5 ms click, then no more capture packets.
    uint32_t shortFrames{};
    for (uint64_t now = 100000; now <= 600000; now += 100000) {
        scheduler.Presented(origin + now, 2, origin + now);
        shortFrames += scheduler.Plan(0, 960, origin + now, true).audio.Frames();
    }
    Require(shortFrames == 240, "prebuffer swallowed a short sound followed by silence");
}
}
int main() {
    try { NormalAndStall(); GenerationsAndSuspend(); RollbackAndBounds(); TimestampBoundary(); LongContinuity(); JitterAndDeferredReset(); }
    catch (const std::exception& error) { std::cerr << "AudioScheduler: " << error.what() << '\n'; return 1; }
    std::cout << "AudioScheduler: 60fps, frozen redraws, jump, reconnect, rollback, discontinuity and bounds passed\n";
}
