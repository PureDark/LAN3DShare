#include "media/AdaptiveBitrate.h"
#include <iostream>
#include <stdexcept>

using lan3d::media::AdaptiveBitrate;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
AdaptiveBitrate::Decision Window(AdaptiveBitrate& rate, unsigned second, unsigned count,
    uint64_t bytes, double sendSeconds, bool burst = false) {
    AdaptiveBitrate::Decision changed{rate.target()};
    for (unsigned i = 0; i < count; ++i) {
        const auto decision = rate.Observe(bytes, burst && i == 0 ? 0.050 : sendSeconds,
            second + double(i + 1) / count);
        if (decision.reason != AdaptiveBitrate::Reason::Hold) changed = decision;
    }
    return changed;
}
}
int main() try {
    AdaptiveBitrate rate;
    rate.Reset(60, 1, 0);
    Require(rate.target() == 20000000, "minimum clamp failed");
    rate.Reset(60, 2000000000, 0);
    Require(rate.target() == 1000000000, "maximum clamp failed");

    rate.Reset(60, 200000000, 0);
    for (unsigned second = 0; second < 10; ++second) Window(rate, second, 60, 1024, 0.0001);
    Require(rate.target() == 200000000, "static low bitrate was mistaken for spare capacity");

    rate.Reset(60, 200000000, 0);
    for (unsigned second = 0; second < 10; ++second) Window(rate, second, 60, 400000, 0.001, true);
    Require(rate.target() >= 200000000, "single 50ms bursts caused a reduction");
    Require(rate.target() <= 200000000 + 3 * 40000000, "probe increases were not gradual");

    rate.Reset(60, 200000000, 0);
    Window(rate, 0, 60, 400000, 0.001);
    Window(rate, 1, 60, 400000, 0.001);
    Require(rate.target() == 200000000, "probe before three stable windows");
    const auto up = Window(rate, 2, 60, 400000, 0.001);
    Require(rate.target() == 220000000 && up.reason == AdaptiveBitrate::Reason::Probe, "loaded healthy link did not probe +10%");

    rate.Reset(60, 200000000, 0);
    Window(rate, 0, 30, 200000, 0.025);
    Require(rate.target() == 200000000, "one congested window caused a reduction");
    const auto down = Window(rate, 1, 30, 200000, 0.025);
    Require(rate.target() >= 47999999 && rate.target() <= 48000001 && down.reason == AdaptiveBitrate::Reason::Congestion,
        "sustained backpressure did not use busy-throughput headroom");
    const auto reduced = rate.target();
    for (unsigned second = 2; second < 7; ++second) Window(rate, second, 60, 400000, 0.001);
    Require(rate.target() == reduced, "post-reduction probe cooldown missing");

    rate.Reset(60, 20000000, 0);
    for (unsigned second = 0; second < 8; ++second) Window(rate, second, 30, 1000, 0.025);
    Require(rate.target() == 20000000, "persistent congestion crossed minimum");
    rate.Reset(60, 980000000, 0);
    for (unsigned second = 0; second < 6; ++second) Window(rate, second, 60, 1600000, 0.001);
    Require(rate.target() == 1000000000, "stable probe crossed maximum");

    rate.Reset(60, 200000000, 0);
    for (unsigned second = 0; second < 6; ++second) Window(rate, second, 60, 400000, 0.006);
    Require(rate.target() == 200000000, "intermediate pressure failed to hold");
    for (unsigned second = 6; second < 12; ++second) Window(rate, second, 1, 400000, 0.5);
    Require(rate.target() == 200000000, "paused/sparse source changed the bitrate");

    rate.Reset(60, 200000000, 0);
    auto timeout = rate.OnSendTimeout(0.25);
    Require(timeout.target == 100000000 && timeout.reason == AdaptiveBitrate::Reason::SendTimeout,
        "send timeout did not immediately halve the target");
    rate.StartStream(60, rate.target(), 1);
    Require(rate.target() == 100000000, "reconnection forgot the reduced target");
    for (unsigned second = 1; second < 6; ++second) Window(rate, second, 60, 400000, 0.001);
    Require(rate.target() == 100000000, "reconnection bypassed timeout cooldown");
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        timeout = rate.OnSendTimeout(10 + attempt);
        rate.StartStream(60, rate.target(), 10.5 + attempt);
    }
    Require(rate.target() == 20000000, "repeated timeouts crossed minimum or reset on reconnect");
    rate.Reset(60, 980000000, 0);
    for (unsigned second = 0; second < 3; ++second) Window(rate, second, 60, 1600000, 0.001);
    rate.StartStream(60, rate.target(), 4);
    Require(rate.target() == 1000000000, "reconnect forgot a successful bounded upward probe");
    std::cout << "PASS: bounds, static-source hold, transient bursts, gradual probing, sustained backpressure, cooldown, sparse-source hold and timeout/reconnect target memory\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
