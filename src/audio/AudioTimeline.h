#pragma once
#include "audio/AudioBlock.h"
#include <algorithm>
#include <deque>

namespace lan3d::audio {
inline constexpr uint64_t UnitsPerSecond = 10000000;
inline uint64_t FrameDuration(uint64_t frames) { return frames * UnitsPerSecond / SampleRate; }
inline uint32_t FramesThrough(uint64_t start, uint64_t end) {
    if (end <= start) return 0;
    const auto delta = end - start;
    if (delta >= FrameDuration(MaxBlockFrames)) return MaxBlockFrames;
    // Invert floor(frames * ticks / rate), including its last fractional frame.
    return static_cast<uint32_t>(((delta + 1) * SampleRate - 1) / UnitsPerSecond);
}

// A short, timestamped queue. No receiver-local clock is compared with sender QPC.
class TimelineQueue {
public:
    uint64_t Clear() {
        const auto frames = Frames(); blocks_.clear(); return frames;
    }
    uint32_t Frames() const {
        uint32_t frames{};
        for (const auto& b : blocks_) frames += b.Remaining();
        return frames;
    }
    uint64_t End() const { return blocks_.empty() ? 0 : blocks_.back().block.End100ns(); }
    uint64_t Push(const Block& block, uint32_t capacityFrames) {
        blocks_.push_back({block, 0});
        uint64_t dropped{};
        auto excess = Frames() > capacityFrames ? Frames() - capacityFrames : 0;
        while (excess && !blocks_.empty()) {
            const auto count = std::min(excess, blocks_.front().Remaining());
            ConsumeFront(count); dropped += count; excess -= count;
        }
        return dropped;
    }
    uint64_t TrimBefore(uint64_t pts) {
        uint64_t dropped{};
        while (!blocks_.empty()) {
            auto& item = blocks_.front(); const auto& block = item.block;
            if (item.Timestamp() >= pts) break;
            if (block.End100ns() <= pts) {
                dropped += item.Remaining(); blocks_.pop_front(); continue;
            }
            const auto through = static_cast<uint32_t>(std::min<uint64_t>(block.Frames(),
                ((pts - block.timestamp100ns) * SampleRate + UnitsPerSecond - 1) / UnitsPerSecond));
            const auto frames = through > item.offset ? through - item.offset : 0;
            dropped += frames; ConsumeFront(frames); break;
        }
        return dropped;
    }
    Block TakeThrough(uint64_t pts, uint32_t maxFrames) {
        Block out;
        maxFrames = std::min(maxFrames, MaxBlockFrames);
        while (!blocks_.empty() && out.Frames() < maxFrames) {
            auto& item = blocks_.front(); const auto& block = item.block;
            const auto throughTotal = FramesThrough(block.timestamp100ns, pts);
            const auto through = throughTotal > item.offset ? throughTotal - item.offset : 0;
            if (!through) break;
            if (!out.samples.empty()) {
                const auto expected = out.End100ns();
                const auto start = item.Timestamp();
                const auto gap = start > expected ? start - expected : expected - start;
                // Do not move sound across a real gap in the captured timeline.
                if ((block.discontinuity && !item.offset) || gap > 10000) break;
            } else {
                out.timestamp100ns = item.Timestamp(); out.discontinuity = block.discontinuity && !item.offset;
            }
            // Also bound the concatenated block: sub-millisecond capture timestamp
            // jitter must not grant samples beyond the last presented picture.
            const auto permittedTotal = FramesThrough(out.timestamp100ns, pts);
            if (permittedTotal <= out.Frames()) break;
            const auto take = std::min({item.Remaining(), through, maxFrames - out.Frames(), permittedTotal - out.Frames()});
            const auto begin = block.samples.begin() + size_t(item.offset) * Channels;
            out.samples.insert(out.samples.end(), begin, begin + size_t(take) * Channels);
            ConsumeFront(take);
        }
        return out;
    }
private:
    void ConsumeFront(uint32_t frames) {
        auto& item = blocks_.front();
        if (frames == item.Remaining()) { blocks_.pop_front(); return; }
        item.offset += frames;
    }
    struct Entry {
        Block block;
        uint32_t offset{};
        uint32_t Remaining() const { return block.Frames() - offset; }
        uint64_t Timestamp() const { return block.timestamp100ns + FrameDuration(offset); }
    };
    std::deque<Entry> blocks_;
};
}
