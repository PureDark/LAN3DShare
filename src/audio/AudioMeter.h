#pragma once
#include "AudioBlock.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace lan3d::audio {
struct Level { float peak{}, rms{}; uint32_t active{}; };
// Caller supplies a local monotonic millisecond clock and holds its existing lock.
// Four 50 ms buckets retain brief transients without retaining any PCM.
class LevelMeter {
public:
    void Reset() { buckets_ = {}; }
    void Add(const Block& block, uint64_t now) {
        if (block.samples.empty()) return;
        auto& bucket = buckets_[(now / 50) % buckets_.size()];
        if (!bucket.count || bucket.tick != now / 50) bucket = {now / 50};
        for (int sample : block.samples) {
            bucket.peak = std::max(bucket.peak, std::abs(sample));
            bucket.squares += double(sample) * sample; ++bucket.count;
        }
    }
    Level Read(uint64_t now) const {
        double squares{}; uint64_t count{}; int peak{};
        for (const auto& b : buckets_) if (b.count && now / 50 >= b.tick && now / 50 - b.tick < 4) {
            squares += b.squares; count += b.count; peak = std::max(peak, b.peak);
        }
        return {float(peak / 32768.0), count ? float(std::sqrt(squares / count) / 32768.0) : 0.f, count ? 1u : 0u};
    }
private:
    struct Bucket { uint64_t tick{}, count{}; double squares{}; int peak{}; };
    std::array<Bucket, 4> buckets_{};
};
}
