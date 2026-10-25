#pragma once
#include "audio/AudioBlock.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace lan3d::audio {
// Receiver-only protection before WASAPI. The fixed 100-frame (2.083 ms)
// delay provides cross-block context and never grows with packet size.
// Frame counts, timestamps, rate and the scheduler's video gate stay intact.
class AudioGain {
public:
    static constexpr unsigned MaxPercent = 800;
    static constexpr int PeakLimit = 32000;
    static constexpr unsigned LookaheadFrames = 96, DelayFrames = LookaheadFrames + 4;
    explicit AudioGain(unsigned percent = 100) : percent_(std::min(percent, MaxPercent)) { Reset(); }
    void SetPercent(unsigned percent) {
        percent = std::min(percent, MaxPercent);
        if (percent == percent_) return;
        percent_ = percent; volumeRamp_ = SampleRate / 200; // 5 ms, including mute and 100%.
    }
    unsigned Percent() const { return percent_; }
    void Reset() {
        Restart();
        volume_ = std::min(1.0, double(percent_) / 100.0);
        volumeRamp_ = percent_ > 100 ? SampleRate / 4 : 0;
    }
    // A short underflow must not undo the user's already established gain.
    void Restart() {
        ring_ = {}; peaks_ = {}; written_ = 0;
        limiter_ = 1; current_ = 0;
    }
    void Apply(Block& block) {
        if (block.samples.empty()) return;
        if (block.discontinuity) Restart();
        const double requested = double(percent_) / 100.0;
        for (size_t frame = 0; frame < block.Frames(); ++frame) {
            const auto n = written_++;
            auto& incoming = At(n);
            for (unsigned ch = 0; ch < Channels; ++ch) incoming[ch] = block.samples[frame * Channels + ch];
            UpdatePeak(n);
            // Four future samples identify isolated one/two-sample spikes before
            // the limiter or speaker sees them. Ring state crosses packet boundaries.
            if (n >= 5) RepairImpulse(n - 3);
            if (volumeRamp_) { volume_ += (requested - volume_) / volumeRamp_; --volumeRamp_; }
            else volume_ = requested;
            if (n < DelayFrames) {
                for (unsigned ch = 0; ch < Channels; ++ch) block.samples[frame * Channels + ch] = 0;
                continue;
            }
            const auto out = n - DelayFrames;
            double ceiling = 1;
            for (unsigned ahead = 0; ahead <= LookaheadFrames; ++ahead) {
                const double peak = peaks_[(out + ahead) % Capacity] * volume_;
                const double safe = peak > PeakLimit ? PeakLimit / peak : 1.0;
                // Anticipate peaks with a continuous attack, independent of blocks.
                ceiling = std::min(ceiling, safe + double(ahead) / LookaheadFrames);
            }
            limiter_ = std::min(ceiling, limiter_ + (1.0 - limiter_) * Release);
            if (limiter_ < 0.999999) ++limitedFrames_;
            const double fade = std::min(1.0, double(out + 1) / LookaheadFrames);
            current_ = volume_ * limiter_ * fade;
            for (unsigned ch = 0; ch < Channels; ++ch)
                block.samples[frame * Channels + ch] = int16_t(std::clamp(
                    std::lround(At(out)[ch] * current_), -long(PeakLimit), long(PeakLimit)));
        }
    }
    double Current() const { return current_; }
    uint64_t LimitedFrames() const { return limitedFrames_; }
    uint64_t RepairedSamples() const { return repairedSamples_; }
private:
    static constexpr size_t Capacity = 128;
    static constexpr double Release = 0.0000833298612076; // 250 ms release at 48 kHz.
    using Stereo = std::array<double, Channels>;
    Stereo& At(uint64_t n) { return ring_[n % Capacity]; }
    void UpdatePeak(uint64_t n) {
        peaks_[n % Capacity] = std::max(std::abs(At(n)[0]), std::abs(At(n)[1]));
    }
    void RepairImpulse(uint64_t n) {
        const double threshold = std::max(2048.0, 12000.0 / std::max({1.0, volume_, double(percent_) / 100.0}));
        const double quiet = std::min(2000.0, threshold / 6.0);
        const double variation = std::min(512.0, quiet / 2.0);
        for (unsigned ch = 0; ch < Channels; ++ch) {
            const double left = At(n - 1)[ch], previous = At(n - 2)[ch];
            if (std::abs(left) > quiet || std::abs(previous) > quiet || std::abs(left - previous) > variation) continue;
            if (std::abs(At(n)[ch] - left) < threshold) continue;
            const unsigned length = std::abs(At(n + 1)[ch]) > quiet ? 2 : 1;
            const double right = At(n + length)[ch], following = At(n + length + 1)[ch];
            if (std::abs(right) > quiet || std::abs(following) > quiet ||
                std::abs(right - following) > variation || std::abs(right - left) > variation) continue;
            if (length == 2 && std::abs(At(n + 1)[ch] - right) < threshold) continue;
            for (unsigned j = 0; j < length; ++j) {
                At(n + j)[ch] = left + (right - left) * double(j + 1) / (length + 1);
                UpdatePeak(n + j); ++repairedSamples_;
            }
        }
    }
    unsigned percent_, volumeRamp_{};
    double volume_{}, limiter_{1}, current_{};
    std::array<Stereo, Capacity> ring_{};
    std::array<double, Capacity> peaks_{};
    uint64_t written_{}, limitedFrames_{}, repairedSamples_{};
};
}
