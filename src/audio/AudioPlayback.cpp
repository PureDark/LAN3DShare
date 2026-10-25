#include "audio/AudioPlayback.h"
#include "audio/AudioWasapi.h"
#include "audio/AudioGain.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace lan3d::audio {
struct Playback::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{false};
    PlaybackScheduler scheduler;
    AudioGain gain;
    LevelMeter receivedMeter, outputMeter;
    unsigned appliedVolumePercent{100};
    bool ready{};
    uint64_t submittedFrames{};
    uint64_t underruns{}, protectionRestarts{};
    uint32_t paddingFrames{}, maxPaddingFrames{};
    std::string status{"stopped"};
    void SetStatus(const std::string& value, bool isReady) {
        std::lock_guard lock(mutex); ready = isReady;
        if (status != value) { status = value; std::cout << "AUDIO_PLAYBACK " << value << '\n'; }
    }
    void Wait(unsigned milliseconds) {
        std::unique_lock lock(mutex); wake.wait_for(lock, std::chrono::milliseconds(milliseconds));
    }
    void Run() {
        try {
            detail::Apartment apartment; detail::AudioPriority priority;
            while (!stop.load()) {
                try {
                    detail::Endpoint endpoint; endpoint.Open(false);
                    detail::ComPtr<IAudioRenderClient> render;
                    detail::Check(endpoint.client->GetService(IID_PPV_ARGS(&render)), "Audio render service");
                    { std::lock_guard lock(mutex); scheduler.EndpointChanged(); gain.Reset(); }
                    SetStatus("ready sample_rate=48000 channels=2 buffer_frames=" + std::to_string(endpoint.bufferFrames), true);
                    uint64_t checkDefault = detail::Now100ns() + 10000000;
                    uint64_t reportAt{}, lastInputEnd{}, submittedUntil{};
                    bool outputActive{};
                    while (!stop.load()) {
                        const auto now = detail::Now100ns();
                        {
                            // Keep the plan and the actual WASAPI submission under the same
                            // lock. A reconnect/reset can never race an old-generation write.
                            std::lock_guard lock(mutex);
                            UINT32 padding{};
                            detail::Check(endpoint.client->GetCurrentPadding(&padding), "Playback padding");
                            auto plan = scheduler.Plan(padding, endpoint.bufferFrames > padding ? endpoint.bufferFrames - padding : 0, now);
                            if (plan.resetEndpoint) {
                                endpoint.Reset(); padding = 0; gain.Reset(); outputMeter.Reset();
                                outputActive = false; lastInputEnd = 0; ++protectionRestarts;
                            } else if (!padding && outputActive && now > submittedUntil) {
                                // Zero padding alone is normal when WASAPI transfers a
                                // quantum to its engine. Require silence longer than our
                                // submitted data plus a full endpoint buffer before reset.
                                gain.Restart(); outputActive = false; lastInputEnd = 0;
                                ++underruns; ++protectionRestarts;
                            }
                            if (!plan.audio.samples.empty()) {
                                const auto pts = plan.audio.timestamp100ns;
                                if (lastInputEnd && (pts > lastInputEnd ? pts - lastInputEnd : lastInputEnd - pts) > 10000) {
                                    gain.Restart(); ++protectionRestarts;
                                }
                                lastInputEnd = plan.audio.End100ns();
                                gain.Apply(plan.audio);
                                appliedVolumePercent = gain.Percent();
                                BYTE* destination{};
                                detail::Check(render->GetBuffer(plan.audio.Frames(), &destination), "Playback buffer");
                                std::memcpy(destination, plan.audio.samples.data(), plan.audio.samples.size() * sizeof(int16_t));
                                detail::Check(render->ReleaseBuffer(plan.audio.Frames(), 0), "Playback submission");
                                submittedFrames += plan.audio.Frames(); padding += plan.audio.Frames();
                                endpoint.Start();
                                submittedUntil = now + FrameDuration(uint64_t(padding) + endpoint.bufferFrames);
                                outputActive = true;
                                outputMeter.Add(plan.audio, GetTickCount64());
                            }
                            paddingFrames = padding; maxPaddingFrames = std::max(maxPaddingFrames, padding);
                            if (now >= reportAt) {
                                std::cout << "AUDIO_PROTECTION repaired_samples=" << gain.RepairedSamples()
                                    << " limited_frames=" << gain.LimitedFrames() << " underruns=" << underruns
                                    << " restarts=" << protectionRestarts << " delay_frames=" << AudioGain::DelayFrames
                                    << " volume_percent=" << gain.Percent() << '\n';
                                reportAt = now + 50000000;
                            }
                        }
                        if (now >= checkDefault) {
                            if (!endpoint.IsCurrentDefault()) throw std::runtime_error("default output changed; reopening");
                            checkDefault = now + 10000000;
                        }
                        Wait(2);
                    }
                    endpoint.Reset();
                } catch (const std::exception& error) {
                    { std::lock_guard lock(mutex); scheduler.EndpointChanged(); paddingFrames = 0; outputMeter.Reset(); }
                    SetStatus(std::string("waiting: ") + error.what(), false); Wait(500);
                }
            }
        } catch (const std::exception& error) { SetStatus(std::string("unavailable: ") + error.what(), false); }
        { std::lock_guard lock(mutex); ready = false; paddingFrames = 0; }
    }
};
Playback::Playback() : impl_(std::make_unique<Impl>()) {}
Playback::~Playback() { Stop(); }
void Playback::Start() {
    if (impl_->worker.joinable()) return;
    impl_->stop = false; impl_->worker = std::thread([this] { impl_->Run(); });
}
void Playback::Stop() {
    if (!impl_->worker.joinable()) return;
    impl_->stop = true; impl_->wake.notify_all(); impl_->worker.join();
    const auto stats = Stats();
    std::cout << "AUDIO_SYNC generation=" << stats.generation << " submitted_frames=" << stats.submittedFrames
        << " accepted_frames=" << stats.acceptedFrames << " dropped_frames=" << stats.droppedFrames
        << " stale_blocks=" << stats.staleBlocks << " resets=" << stats.resets << " stalls=" << stats.stalls
        << " queued_frames=" << stats.queuedFrames << " max_queue_frames=" << stats.maxQueueFrames
        << " max_ahead100ns=" << stats.maxAhead100ns << " max_padding_frames=" << stats.maxPaddingFrames
        << " volume_percent=" << stats.volumePercent << " applied_volume_percent=" << stats.appliedVolumePercent
        << " repaired_samples=" << stats.repairedSamples << " limited_frames=" << stats.limitedFrames
        << " underruns=" << stats.underruns << " protection_restarts=" << stats.protectionRestarts << '\n';
    std::lock_guard lock(impl_->mutex); impl_->status = "stopped";
}
void Playback::Reset(uint64_t generation) {
    { std::lock_guard lock(impl_->mutex); impl_->scheduler.Reset(generation); impl_->gain.Reset(); impl_->receivedMeter.Reset(); impl_->outputMeter.Reset(); }
    impl_->wake.notify_all();
}
void Playback::Push(const Block& block, uint64_t generation) {
    { std::lock_guard lock(impl_->mutex); impl_->scheduler.Push(block, generation);
      if (generation == impl_->scheduler.Stats().generation && ValidBlock(block)) impl_->receivedMeter.Add(block, GetTickCount64()); }
    impl_->wake.notify_all();
}
void Playback::Presented(uint64_t videoPts, uint64_t generation) {
    { std::lock_guard lock(impl_->mutex); impl_->scheduler.Presented(videoPts, generation, detail::Now100ns()); }
    impl_->wake.notify_all();
}
void Playback::Suspend() {
    { std::lock_guard lock(impl_->mutex); impl_->scheduler.Suspend(); impl_->outputMeter.Reset(); }
    impl_->wake.notify_all();
}
void Playback::SetVolume(unsigned percent) {
    { std::lock_guard lock(impl_->mutex); impl_->gain.SetPercent(percent); }
    impl_->wake.notify_all();
}
std::string Playback::Status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
PlaybackStats Playback::Stats() const {
    std::lock_guard lock(impl_->mutex);
    PlaybackStats stats; static_cast<SyncStats&>(stats) = impl_->scheduler.Stats();
    stats.ready = impl_->ready; stats.submittedFrames = impl_->submittedFrames;
    stats.paddingFrames = impl_->paddingFrames; stats.maxPaddingFrames = impl_->maxPaddingFrames;
    stats.repairedSamples = impl_->gain.RepairedSamples(); stats.limitedFrames = impl_->gain.LimitedFrames();
    stats.underruns = impl_->underruns; stats.protectionRestarts = impl_->protectionRestarts;
    stats.receivedLevel = impl_->receivedMeter.Read(GetTickCount64()); stats.outputLevel = impl_->outputMeter.Read(GetTickCount64());
    stats.volumePercent = impl_->gain.Percent(); stats.appliedVolumePercent = impl_->appliedVolumePercent; return stats;
}
}
