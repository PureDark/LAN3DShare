#include "audio/AudioMeter.h"
#include "audio/CaptureClock.h"
#include "audio/AudioTelemetry.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lan3d::audio;
void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    try {
        Block block; block.timestamp100ns = 10000000; block.samples = {-32768, 32767, 0, 0};
        const auto original = block.samples;
        LevelMeter meter; meter.Add(block, 1000);
        auto value = meter.Read(1000);
        Require(value.peak == 1 && value.rms > .70f && value.rms < .71f && value.active, "peak/RMS full scale or negative S16 failed");
        Require(meter.Read(1150).peak == 1 && !meter.Read(1200).active, "meter retained stale audio or missed transient");
        Require(block.samples == original, "meter modified PCM");
        block.samples = {0,0}; meter.Add(block, 1250);
        Require(meter.Read(1250).active && !meter.Read(1250).peak, "silent packet indistinguishable from absent packet");
        meter.Reset(); Require(!meter.Read(1250).active, "reset retained audio level");
        CaptureClock clock;
        constexpr uint64_t now = 5495335260514, badOffset = 429496729600000;
        auto time = clock.Map(now + 50000, 0, now, 480, false);
        Require(time.timestamp == now + 50000 && !time.corrected, "healthy endpoint lookahead changed");
        time = clock.Map(now + badOffset, 480, now, 480, false);
        Require(time.corrected && time.reanchored && time.timestamp == now - 100000, "bad clock epoch not anchored");
        time = clock.Map(now + badOffset + 100000, 960, now + 130000, 480, false);
        Require(time.timestamp == now && !time.reanchored, "arrival jitter changed sample timing");
        time = clock.Map(now + badOffset, 0, now + 200000, 480, false);
        Require(time.reanchored && time.timestamp == now + 100000, "device position rollback not reanchored");
        time = clock.Map(now + 250000, 480, now + 300000, 480, false);
        Require(!time.corrected && time.reanchored && time.timestamp == now + 250000, "healthy clock recovery not signaled");
        Telemetry read;
        Require(!ReadTelemetry(0, read), "accepted missing process");
        std::atomic<unsigned> revision{1};
        {
            TelemetryPublisher publisher([&] { Telemetry data; data.ready = 1; data.enabled = 1; data.inputFrames = revision.load(); data.input = {0.5f, 0.25f, 1}; return data; });
            auto until = GetTickCount64() + 1500;
            while (GetTickCount64() < until && (!ReadTelemetry(GetCurrentProcessId(), read) || read.inputFrames != 1)) Sleep(10);
            Require(read.inputFrames == 1 && read.input.peak == .5f, "shared telemetry roundtrip failed");
            revision = 2; until = GetTickCount64() + 1500;
            while (GetTickCount64() < until && (!ReadTelemetry(GetCurrentProcessId(), read) || read.inputFrames != 2)) Sleep(10);
            Require(read.inputFrames == 2, "shared telemetry did not update");
        }
        Require(!ReadTelemetry(GetCurrentProcessId(), read), "disconnected publisher retained levels");
        std::cout << "AudioMeter passed: PCM peak/RMS/silence/expiry, clock epoch/jitter/rollback/recovery, shared telemetry refresh/teardown\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
