#include "audio/AudioCapture.h"
#include "audio/AudioPlayback.h"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace lan3d::audio;
namespace {
uint64_t Now() {
    LARGE_INTEGER value{}, frequency{}; QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return uint64_t(value.QuadPart / frequency.QuadPart) * 10000000 +
        uint64_t(value.QuadPart % frequency.QuadPart) * 10000000 / uint64_t(frequency.QuadPart);
}
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Block Silence(uint64_t pts) { Block block; block.timestamp100ns = pts; block.samples.resize(480 * Channels); return block; }
void Feed(Playback& playback, uint64_t& pts, uint64_t generation, unsigned iterations, bool present) {
    for (unsigned i = 0; i < iterations; ++i) {
        playback.Push(Silence(pts), generation); pts += 100000;
        if (present) playback.Presented(pts, generation);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
int main() {
    LoopbackCapture capture; Playback playback;
    try {
        capture.Start(); playback.Start(); playback.Reset(1);
        const auto deadline = Now() + 40000000;
        while (Now() < deadline && (!capture.Stats().ready || !playback.Stats().ready))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!capture.Stats().ready || !playback.Stats().ready) {
            std::cout << "AudioEndpoint unavailable capture=" << capture.Status() << " playback=" << playback.Status() << '\n';
            playback.Stop(); capture.Stop(); return 77;
        }
        uint64_t pts = Now(); Feed(playback, pts, 1, 100, true);
        const auto running = playback.Stats();
        Require(running.submittedFrames > 30000, "silent WASAPI playback did not submit steadily");
        Require(running.maxPaddingFrames <= PlaybackScheduler::MaxEndpointFrames, "endpoint padding exceeded 20 ms");
        for (unsigned volume : {500u, 0u, 100u, 800u}) {
            const auto before = playback.Stats();
            playback.SetVolume(volume);
            Feed(playback, pts, 1, 10, true);
            const auto after = playback.Stats();
            Require(after.volumePercent == volume && after.appliedVolumePercent == volume,
                "live playback volume was not applied to the next buffers");
            Require(after.generation == before.generation && after.resets == before.resets &&
                after.submittedFrames > before.submittedFrames, "volume reset or interrupted playback");
        }
        Feed(playback, pts, 1, 10, false);
        const auto frozen = playback.Stats();
        playback.SetVolume(600);
        Feed(playback, pts, 1, 20, false);
        Require(playback.Stats().submittedFrames == frozen.submittedFrames, "sound submission continued with frozen video");
        Require(playback.Stats().paddingFrames == 0, "video freeze retained queued endpoint audio");
        playback.Reset(2);
        Feed(playback, pts, 1, 10, true);
        Require(playback.Stats().submittedFrames == frozen.submittedFrames, "old generation resumed after reconnect");
        Feed(playback, pts, 2, 30, true);
        const auto resumed = playback.Stats();
        Require(resumed.submittedFrames > frozen.submittedFrames + 8000, "fresh generation failed to recover playback");
        Require(resumed.maxAhead100ns == 0 && resumed.staleBlocks >= 10, "generation or presentation budget violated");
        Require(resumed.volumePercent == 600 && resumed.appliedVolumePercent == 600, "reconnect lost receiver volume");
        const auto captureStats = capture.Stats();
        if (captureStats.packets) {
            const auto now = Now();
            std::cout << "AudioEndpoint timestamp_probe capture_qpc100ns=" << captureStats.latestTimestamp100ns
                << " now_qpc100ns=" << now << " delta100ns=" << int64_t(captureStats.latestTimestamp100ns) - int64_t(now)
                << " capture_buffer_frames=" << captureStats.endpointBufferFrames << " stream_latency100ns=" << captureStats.streamLatency100ns
                << " max_capture_lead100ns=" << captureStats.maxCaptureLead100ns << '\n';
            // Loopback taps the render engine before the hardware consumes its
            // queued buffer: this endpoint reports first-sample QPC up to about
            // 11 ms in the future (22 ms buffer). Keep that presentation timestamp;
            // arrival time is not the audio timeline. Bound it by the actual
            // endpoint buffer/latency and verify extraction still obeys video PTS.
            const auto endpointLookahead = FrameDuration(captureStats.endpointBufferFrames) + captureStats.streamLatency100ns;
            Require((captureStats.latestTimestamp100ns <= now || captureStats.latestTimestamp100ns - now <= endpointLookahead) &&
                (now <= captureStats.latestTimestamp100ns || now - captureStats.latestTimestamp100ns < 20000000) &&
                captureStats.maxCaptureLead100ns <= endpointLookahead,
                "capture QPC exceeds endpoint lookahead or does not match sender timestamp epoch");
            const auto block = capture.TakeThrough(now);
            Require(!block.samples.empty(), "captured audio cannot be extracted on the video clock (bad endpoint epoch)");
            Require(ValidBlock(block) && block.Frames() <= SampleRate * 80 / 1000 &&
                (block.samples.empty() || block.End100ns() <= now), "captured block is invalid, unbounded or ahead of video");
        }
        std::cout << "AudioEndpoint capture_ready=1 capture_packets=" << captureStats.packets
            << " captured_frames=" << captureStats.capturedFrames << " submitted_frames=" << resumed.submittedFrames
            << " clock_corrected=" << captureStats.clockCorrected << " clock_corrections=" << captureStats.clockCorrections
            << " freeze_submitted_delta=0 stale_generation_rejected=1 reconnect_resumed=1 max_ahead100ns=" << resumed.maxAhead100ns
            << " max_padding_frames=" << resumed.maxPaddingFrames << " live_volume_updates=4 volume_survived_reset=600 (silence only; no acoustic validation)\n";
        playback.Stop(); capture.Stop(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "AudioEndpoint: " << error.what() << '\n'; playback.Stop(); capture.Stop(); return 1;
    }
}
