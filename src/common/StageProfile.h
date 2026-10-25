#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace lan3d {
class StageProfile {
public:
    using Clock = std::chrono::steady_clock;
    StageProfile(bool enabled, const char* label, std::initializer_list<const char*> names)
        : enabled_(enabled), label_(label), names_(names), begin_(Clock::now()), window_(begin_) {}
    ~StageProfile() { Print(true); }
    void Add(size_t index, double ms) {
        if (!enabled_ || index >= names_.size()) return;
        for (auto* set : {&windowSamples_, &totals_}) {
            auto& sample = (*set)[index]; ++sample.count; sample.sum += ms; sample.maximum = std::max(sample.maximum, ms);
            if (sample.values.size() < 8192) sample.values.push_back(ms);
            else sample.values[(sample.count - 1) % 8192] = ms;
        }
    }
    void Record(size_t index, Clock::time_point start) { Add(index, std::chrono::duration<double, std::milli>(Clock::now() - start).count()); }
    void Print(bool summary = false) {
        if (!enabled_) return;
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - (summary ? begin_ : window_)).count();
        if (!summary && seconds < 1) return;
        auto& set = summary ? totals_ : windowSamples_;
        std::string line = label_ + (summary ? "_SUMMARY" : "") + " seconds=" + std::to_string(seconds);
        for (size_t i = 0; i < names_.size(); ++i) {
            auto& sample = set[i]; auto sorted = sample.values; std::sort(sorted.begin(), sorted.end());
            const double p95 = sorted.empty() ? 0 : sorted[std::min(sorted.size()-1, size_t(sorted.size() * .95))];
            line += " " + std::string(names_[i]) + "_ms=" + std::to_string(sample.count ? sample.sum / sample.count : 0) +
                "/" + std::to_string(p95) + "/" + std::to_string(sample.maximum) + "/" + std::to_string(sample.count);
        }
        std::cout << line << " units=mean/p95/max/count p95_last8192=1\n" << std::flush;
        if (!summary) { windowSamples_ = {}; window_ = now; }
    }
private:
    struct Samples { uint64_t count{}; double sum{}, maximum{}; std::vector<double> values; };
    bool enabled_; std::string label_; std::vector<const char*> names_;
    Clock::time_point begin_, window_;
    std::array<Samples, 8> windowSamples_, totals_;
};
}
