#pragma once
#include <cstdint>
#include <vector>

namespace lan3d::audio {
inline constexpr uint32_t SampleRate = 48000, Channels = 2;
inline constexpr uint32_t MaxBlockFrames = SampleRate / 10; // 100 ms; never replay a backlog.
struct Block {
    uint64_t timestamp100ns{}; // Sender QPC, first interleaved stereo sample frame.
    std::vector<int16_t> samples;
    bool discontinuity{};
    uint32_t Frames() const { return static_cast<uint32_t>(samples.size() / Channels); }
    uint64_t End100ns() const { return timestamp100ns + uint64_t(Frames()) * 10000000 / SampleRate; }
};
inline bool ValidBlock(const Block& block) {
    return block.samples.size() % Channels == 0 && block.samples.size() <= size_t(MaxBlockFrames) * Channels &&
        (block.samples.empty() ? block.timestamp100ns == 0 && !block.discontinuity :
            block.timestamp100ns != 0 && block.End100ns() >= block.timestamp100ns);
}
}
