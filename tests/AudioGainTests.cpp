#include "audio/AudioGain.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace lan3d::audio;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Pcm = std::vector<int16_t>;
Pcm Constant(size_t frames, int left, int right) {
    Pcm pcm(frames * Channels);
    for (size_t f = 0; f < frames; ++f) { pcm[f * 2] = int16_t(left); pcm[f * 2 + 1] = int16_t(right); }
    return pcm;
}
Pcm Tone(size_t frames, double hz, int amplitude) {
    auto pcm = Constant(frames, 0, 0);
    for (size_t f = 0; f < frames; ++f) {
        pcm[f * 2] = int16_t(std::lround(amplitude * std::sin(6.283185307179586 * hz * double(f) / SampleRate)));
        pcm[f * 2 + 1] = int16_t(-pcm[f * 2]);
    }
    return pcm;
}
Pcm Process(AudioGain& gain, const Pcm& input, unsigned chunk = 480) {
    Pcm output;
    for (size_t first = 0; first < input.size() / Channels; first += chunk) {
        const auto frames = std::min<size_t>(chunk, input.size() / Channels - first);
        Block block; block.timestamp100ns = 123456789 + first * 10000000 / SampleRate;
        block.samples.assign(input.begin() + first * Channels, input.begin() + (first + frames) * Channels);
        const auto pts = block.timestamp100ns, end = block.End100ns();
        gain.Apply(block);
        Require(block.timestamp100ns == pts && block.End100ns() == end && block.Frames() == frames && !block.discontinuity,
            "processor changed timing, frame count or discontinuity");
        output.insert(output.end(), block.samples.begin(), block.samples.end());
    }
    return output;
}
void NormalSignals() {
    // Bypass means unity amplitude after the documented fixed delay/fade,
    // not bypassing protection or instantly switching gains at 100%.
    for (const double hz : {40.0, 997.0, 12000.0, 18000.0, 23000.0}) {
        AudioGain gain;
        const auto input = Tone(4800, hz, 28000), output = Process(gain, input, 37);
        Require(gain.RepairedSamples() == 0, "legitimate tone mistaken for impulse");
        for (size_t f = 200; f < input.size() / Channels; ++f)
            for (size_t ch = 0; ch < Channels; ++ch)
                Require(output[f * Channels + ch] == input[(f - AudioGain::DelayFrames) * Channels + ch], "unity tone altered");
    }
    // A sustained attack is not an isolated corruption, even from silence.
    auto input = Constant(4800, 0, 0);
    for (size_t f = 480; f < 960; ++f) { input[f * 2] = 30000; input[f * 2 + 1] = -30000; }
    AudioGain gain; const auto output = Process(gain, input);
    Require(gain.RepairedSamples() == 0 && output[(600 + AudioGain::DelayFrames) * 2] == 30000,
        "sustained transient removed");
}
void ImpulsesAcrossBlocks() {
    auto input = Constant(4800, 100, -100);
    for (const auto f : {479u, 480u, 1439u, 2879u, 2880u}) { input[f * 2] = 32767; input[f * 2 + 1] = -32768; }
    AudioGain gain;
    const auto output = Process(gain, input);
    Require(gain.RepairedSamples() == 10, "isolated/cross-block impulses were not repaired");
    for (size_t f = 200; f < output.size() / Channels; ++f)
        Require(output[f * 2] == 100 && output[f * 2 + 1] == -100, "impulse leaked or clean context damaged");
    AudioGain boosted(800);
    input = Constant(4800, 0, 0); input[479 * 2] = 3000;
    const auto repaired = Process(boosted, input);
    Require(boosted.RepairedSamples() == 1 && std::all_of(repaired.begin(), repaired.end(), [](auto x) { return x == 0; }),
        "amplification-aware impulse detection failed");
}
void PacketInvariantLimiter() {
    auto input = Tone(48000, 997, 500);
    // Slow ramp, full-scale bursts and sign extremes exercise actual limiting.
    for (size_t f = 8000; f < 10000; ++f) { input[f * 2] = int16_t((f - 8000) * 15); input[f * 2 + 1] = int16_t(-int(input[f * 2])); }
    for (size_t f = 20000; f < 21000; ++f) { input[f * 2] = f % 2 ? 32767 : -32768; input[f * 2 + 1] = input[f * 2]; }
    AudioGain a(800), b(800), c(800);
    const auto whole = Process(a, input, MaxBlockFrames);
    Require(whole == Process(b, input, 37) && whole == Process(c, input, 1), "packet boundaries change protection envelope");
    Require(a.LimitedFrames() > 0, "overload did not engage limiter");
    for (auto sample : whole) Require(std::abs(int(sample)) <= AudioGain::PeakLimit, "overload clipped/wrapped");
    // Continuous input at a block boundary followed by a later loud peak used
    // to make the old whole-block limiter jump immediately at that boundary.
    AudioGain gain(800); Process(gain, Constant(24000, 1000, -500));
    auto block = Constant(480, 1000, -500);
    for (size_t f = 300; f < 400; ++f) { block[f * 2] = 30000; block[f * 2 + 1] = -15000; }
    const auto result = Process(gain, block);
    Require(std::abs(int(result[0]) - 8000) <= 1, "limiter created a gain step at a packet boundary");
    for (size_t f = 1; f < 390; ++f)
        Require(std::abs(int(result[f * 2]) - int(result[(f - 1) * 2])) < 200, "anticipatory attenuation is discontinuous");
    for (size_t f = 0; f < result.size() / 2; ++f)
        Require(std::abs(int(result[f * 2]) + 2 * int(result[f * 2 + 1])) <= 2, "stereo balance changed");
}
void VolumeAndReset() {
    AudioGain gain;
    Process(gain, Constant(480, 1000, -500));
    int last = 1000;
    for (const auto percent : {600u, 0u, 100u, 800u, 50u}) {
        gain.SetPercent(percent);
        const auto result = Process(gain, Constant(960, 1000, -500), 31);
        for (size_t f = 0; f < result.size() / 2; ++f) {
            Require(std::abs(int(result[f * 2]) - last) <= 34, "live volume/mute made an abrupt step");
            last = result[f * 2];
        }
        Require(last == int(percent * 10), "live volume did not converge in 5 ms");
    }
    gain.SetPercent(600); Process(gain, Constant(480, 20000, -10000)); gain.Reset();
    Require(gain.Percent() == 600, "reset lost selected volume");
    const auto silence = Process(gain, Constant(480, 0, 0));
    Require(std::all_of(silence.begin(), silence.end(), [](auto x) { return x == 0; }), "reconnect replayed old delayed audio");
    gain.SetPercent(50000); Require(gain.Percent() == 800, "volume cap broken");
    AudioGain mute(0), half(50), quiet(600);
    const auto muted = Process(mute, Constant(480, -32768, 32767));
    Require(std::all_of(muted.begin(), muted.end(), [](auto x) { return x == 0; }), "initial mute leaked");
    Require(Process(half, Constant(480, 1000, -1000)).back() == -500, "attenuation failed");
    const auto boosted = Process(quiet, Constant(48000, 1000, -500));
    Require(boosted[boosted.size() - 2] == 6000 && boosted.back() == -3000, "quiet sound amplification failed");
    quiet.Restart();
    Require(Process(quiet, Constant(480, 1000, -500)).back() == -3000, "short underflow lost established volume");
    Block discontinuous; discontinuous.timestamp100ns = 987654321;
    discontinuous.discontinuity = true; discontinuous.samples = Constant(480, 0, 0);
    quiet.Apply(discontinuous);
    Require(discontinuous.timestamp100ns == 987654321 && discontinuous.discontinuity && discontinuous.Frames() == 480 &&
        std::all_of(discontinuous.samples.begin(), discontinuous.samples.end(), [](auto x) { return x == 0; }),
        "discontinuity replayed old samples or changed metadata");
    Block empty; gain.Apply(empty); Require(ValidBlock(empty), "empty block corrupted");
}
}
int main() try {
    const auto start = std::chrono::steady_clock::now();
    NormalSignals(); ImpulsesAcrossBlocks(); PacketInvariantLimiter(); VolumeAndReset();
    std::cout << "AudioGain passed: clean tones/transients, cross-block impulse repair, packet-invariant peak protection, stereo, smooth live volume/mute, timing and reset; delay_frames="
        << AudioGain::DelayFrames << " test_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << '\n';
    return 0;
} catch (const std::exception& error) { std::cerr << "AudioGain: " << error.what() << '\n'; return 1; }
