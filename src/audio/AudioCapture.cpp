#include "audio/AudioCapture.h"
#include "audio/AudioTimeline.h"
#include "audio/CaptureClock.h"
#include "audio/AudioWasapi.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace lan3d::audio {
struct LoopbackCapture::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{false};
    TimelineQueue queue;
    CaptureStats stats;
    LevelMeter meter;
    std::string status{"stopped"};
    bool discontinuity{true};
    void SetStatus(const std::string& value, bool ready) {
        std::lock_guard lock(mutex); stats.ready = ready;
        if (status != value) { status = value; std::cout << "AUDIO_CAPTURE " << value << '\n'; }
    }
    void Wait(unsigned milliseconds) {
        std::unique_lock lock(mutex); wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return stop.load(); });
    }
    void Run() {
        try {
            detail::Apartment apartment; detail::AudioPriority priority;
            while (!stop.load()) {
                try {
                    detail::Endpoint endpoint; endpoint.Open(true);
                    detail::ComPtr<IAudioCaptureClient> capture;
                    detail::Check(endpoint.client->GetService(IID_PPV_ARGS(&capture)), "Loopback capture service");
                    endpoint.Start();
                    CaptureClock clock;
                    { std::lock_guard lock(mutex);
                      stats.endpointBufferFrames = endpoint.bufferFrames;
                      stats.streamLatency100ns = static_cast<uint64_t>(endpoint.streamLatency); }
                    SetStatus("ready sample_rate=48000 channels=2 format=s16", true);
                    uint64_t checkDefault = detail::Now100ns() + 10000000;
                    while (!stop.load()) {
                        UINT32 available{};
                        detail::Check(capture->GetNextPacketSize(&available), "Loopback next packet");
                        while (available && !stop.load()) {
                            BYTE* data{}; UINT32 frames{}; DWORD flags{}; UINT64 devicePosition{}, qpc{};
                            detail::Check(capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc), "Loopback packet");
                            const auto readQpc = detail::Now100ns();
                            try {
                                // WASAPI returns first-sample QPC already converted to 100 ns,
                                // the same unit/epoch as common::Timestamp100ns. A reported
                                // timestamp error is discarded; arrival time is not a substitute.
                                std::lock_guard lock(mutex);
                                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !qpc) {
                                    stats.discardedFrames += frames; ++stats.timestampErrors; discontinuity = true;
                                } else {
                                    const auto time = clock.Map(qpc, devicePosition, readQpc, frames, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0);
                                    stats.rawTimestamp100ns = qpc; stats.clockCorrected = time.corrected;
                                    if (time.reanchored) {
                                        ++stats.clockCorrections; discontinuity = true;
                                        std::cout << "AUDIO_CLOCK reanchored raw=" << qpc << " local=" << readQpc << " mapped=" << time.timestamp << " device_position=" << devicePosition << '\n';
                                    }
                                    qpc = time.timestamp;
                                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuity = true;
                                    if (discontinuity) stats.discardedFrames += queue.Clear();
                                    for (UINT32 offset = 0; offset < frames;) {
                                        const auto count = std::min<UINT32>(frames - offset, MaxBlockFrames);
                                        Block block; block.timestamp100ns = qpc + FrameDuration(offset);
                                        block.samples.resize(size_t(count) * Channels);
                                        block.discontinuity = discontinuity; discontinuity = false;
                                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
                                            std::memcpy(block.samples.data(), data + size_t(offset) * Channels * sizeof(int16_t),
                                                size_t(count) * Channels * sizeof(int16_t));
                                        meter.Add(block, GetTickCount64());
                                        stats.discardedFrames += queue.Push(block, SampleRate * 150 / 1000);
                                        stats.capturedFrames += count; stats.latestTimestamp100ns = block.timestamp100ns;
                                        stats.latestRead100ns = readQpc;
                                        if (block.timestamp100ns > readQpc)
                                            stats.maxCaptureLead100ns = std::max(stats.maxCaptureLead100ns, block.timestamp100ns - readQpc);
                                        offset += count;
                                    }
                                    ++stats.packets;
                                }
                            } catch (...) { capture->ReleaseBuffer(frames); throw; }
                            detail::Check(capture->ReleaseBuffer(frames), "Release loopback packet");
                            detail::Check(capture->GetNextPacketSize(&available), "Loopback next packet");
                        }
                        const auto now = detail::Now100ns();
                        if (now >= checkDefault) {
                            if (!endpoint.IsCurrentDefault()) throw std::runtime_error("default output changed; reopening");
                            checkDefault = now + 10000000;
                        }
                        Wait(3);
                    }
                } catch (const std::exception& error) {
                    { std::lock_guard lock(mutex); stats.discardedFrames += queue.Clear(); meter.Reset(); discontinuity = true; }
                    SetStatus(std::string("waiting: ") + error.what(), false); Wait(500);
                }
            }
        } catch (const std::exception& error) { SetStatus(std::string("unavailable: ") + error.what(), false); }
        { std::lock_guard lock(mutex); stats.ready = false; }
    }
};
LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { Stop(); }
void LoopbackCapture::Start() {
    if (impl_->worker.joinable()) return;
    impl_->stop = false; impl_->worker = std::thread([this] { impl_->Run(); });
}
void LoopbackCapture::Stop() {
    if (!impl_->worker.joinable()) return;
    impl_->stop = true; impl_->wake.notify_all(); impl_->worker.join();
    std::lock_guard lock(impl_->mutex);
    impl_->stats.discardedFrames += impl_->queue.Clear(); impl_->status = "stopped";
    std::cout << "AUDIO_CAPTURE stopped captured_frames=" << impl_->stats.capturedFrames
        << " discarded_frames=" << impl_->stats.discardedFrames << " timestamp_errors=" << impl_->stats.timestampErrors << '\n';
}
Block LoopbackCapture::TakeThrough(uint64_t videoPts) {
    std::lock_guard lock(impl_->mutex);
    if (videoPts > 800000) impl_->stats.discardedFrames += impl_->queue.TrimBefore(videoPts - 800000);
    return impl_->queue.TakeThrough(videoPts, SampleRate * 80 / 1000);
}
std::string LoopbackCapture::Status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
CaptureStats LoopbackCapture::Stats() const {
    std::lock_guard lock(impl_->mutex); auto stats = impl_->stats; stats.queuedFrames = impl_->queue.Frames(); stats.level = impl_->meter.Read(GetTickCount64()); return stats;
}
}
