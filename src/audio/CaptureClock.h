#pragma once
#include "AudioTimeline.h"

namespace lan3d::audio {
// Healthy WASAPI timestamps pass through unchanged. Some endpoints return a
// different epoch without TIMESTAMP_ERROR; anchor their device sample clock to
// local QPC once, not to packet arrival on every read.
class CaptureClock {
public:
    struct Result { uint64_t timestamp{}; bool corrected{}, reanchored{}; };
    Result Map(uint64_t raw, uint64_t position, uint64_t now, uint32_t frames, bool discontinuity) {
        const bool plausible = raw && (raw > now ? raw - now : now - raw) <= 20000000;
        if (plausible) { const bool changed = fallback_; fallback_ = false; return {raw, false, changed}; }
        bool anchor = !fallback_ || discontinuity || position < position_;
        uint64_t mapped{};
        if (!anchor) {
            const auto delta = position - position_;
            if (delta > uint64_t(SampleRate) * 86400) anchor = true;
            else {
                mapped = time_ + FrameDuration(delta);
                if ((mapped > now && mapped - now > 1000000) || (now > mapped && now - mapped > 20000000)) anchor = true;
            }
        }
        if (anchor) { position_ = position; time_ = now > FrameDuration(frames) ? now - FrameDuration(frames) : 1; mapped = time_; }
        fallback_ = true; return {mapped, true, anchor};
    }
private:
    bool fallback_{};
    uint64_t position_{}, time_{};
};
}
