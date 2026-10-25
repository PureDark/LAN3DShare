#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lan3d::media {

// A conservative TCP backpressure controller, not a link-bandwidth measurement.
// Send duration includes socket-buffer and receiver-consumption backpressure.
// Low-complexity video is never treated as evidence of spare link capacity.
class AdaptiveBitrate {
public:
    static constexpr uint32_t Minimum = 20000000;
    static constexpr uint32_t Maximum = 1000000000;
    static constexpr uint32_t Initial = 200000000;
    enum class Reason { Hold, Congestion, Probe, SendTimeout };
    struct Decision {
        uint32_t target{};
        Reason reason{Reason::Hold};
        double payloadBitsPerSecond{};
        double sendFraction{};
    };

    void Reset(uint32_t fps, uint32_t target, double nowSeconds) {
        StartStream(fps, target, nowSeconds);
        lastReduction_ = nowSeconds - 5;
    }
    // Reset observations on a new encoder/socket, retaining the last reduction's
    // cooldown and the supplied actual target instead of returning to 200 Mbps.
    void StartStream(uint32_t fps, uint32_t target, double nowSeconds) {
        fps_ = std::clamp(fps, 1u, 240u);
        target_ = std::clamp(target, Minimum, Maximum);
        windowStart_ = nowSeconds;
        congestedWindows_ = stableWindows_ = 0;
        ClearWindow();
    }
    uint32_t target() const { return target_; }

    Decision OnSendTimeout(double nowSeconds) {
        target_ = std::max(Minimum, target_ / 2);
        StartStream(fps_, target_, nowSeconds);
        lastReduction_ = nowSeconds;
        return {target_, Reason::SendTimeout};
    }

    Decision Observe(uint64_t wireBytes, double sendSeconds, double nowSeconds) {
        Decision result{target_};
        if (!std::isfinite(sendSeconds) || !std::isfinite(nowSeconds) || sendSeconds < 0 || nowSeconds < windowStart_)
            return result;
        bytes_ += wireBytes;
        sendTime_ += sendSeconds;
        ++frames_;
        const double frameBudget = 1.0 / fps_;
        if (sendSeconds > frameBudget * 0.75) ++slowFrames_;
        const double elapsed = nowSeconds - windowStart_;
        if (elapsed < 1.0) return result;
        result.payloadBitsPerSecond = double(bytes_) * 8 / elapsed;
        result.sendFraction = sendTime_ / elapsed;
        // A sparse/paused source or a long observation gap is not a network probe.
        if (frames_ < 4 || elapsed > 2.5) {
            congestedWindows_ = stableWindows_ = 0;
        } else {
            const bool congested = result.sendFraction > 0.40 || double(slowFrames_) / frames_ >= 0.30;
            if (congested) {
                stableWindows_ = 0;
                if (++congestedWindows_ >= 2) {
                    // Only use send-time throughput once sustained blocking supplies
                    // evidence. The socket may buffer a short burst faster than the wire.
                    const double busyThroughput = double(bytes_) * 8 / std::max(sendTime_, 0.001);
                    const double reduced = std::min(double(target_) * 0.80, busyThroughput * 0.75);
                    const auto next = static_cast<uint32_t>(std::clamp(reduced, double(Minimum), double(Maximum)));
                    if (next < target_) {
                        target_ = next;
                        result.reason = Reason::Congestion;
                        lastReduction_ = nowSeconds;
                    }
                    congestedWindows_ = 0;
                }
            } else {
                congestedWindows_ = 0;
                const bool loaded = result.payloadBitsPerSecond >= double(target_) * 0.60;
                const bool spare = sendTime_ / frames_ < frameBudget * 0.30 && result.sendFraction < 0.25
                    && double(slowFrames_) / frames_ < 0.05;
                if (loaded && spare && nowSeconds - lastReduction_ >= 5.0) {
                    if (++stableWindows_ >= 3) {
                        const auto next = static_cast<uint32_t>(std::min<uint64_t>(Maximum,
                            uint64_t(target_) + std::max(target_ / 10, 10000000u)));
                        if (next > target_) { target_ = next; result.reason = Reason::Probe; }
                        stableWindows_ = 0;
                    }
                } else stableWindows_ = 0;
            }
        }
        result.target = target_;
        windowStart_ = nowSeconds;
        ClearWindow();
        return result;
    }

private:
    void ClearWindow() { bytes_ = 0; frames_ = slowFrames_ = 0; sendTime_ = 0; }
    uint32_t fps_{60}, target_{Initial}, frames_{}, slowFrames_{};
    unsigned congestedWindows_{}, stableWindows_{};
    uint64_t bytes_{};
    double windowStart_{}, lastReduction_{-5}, sendTime_{};
};

}
