#pragma once
#include "common/Frame.h"
#include <memory>

namespace lan3d::ui {
struct PreviewImage {
    Frame frame;
    uint32_t sourceWidth{}, sourceHeight{};
};

// A small, independent, best-effort channel for the owning client's cards.
// The full SBS image is preserved; no runtime or encoder state is changed.
class PreviewPublisher {
public:
    PreviewPublisher();
    ~PreviewPublisher();
    PreviewPublisher(const PreviewPublisher&) = delete;
    PreviewPublisher& operator=(const PreviewPublisher&) = delete;
    void Publish(const Frame& frame) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns false without changing image if absent, busy, or malformed. Callers
// supply only their own currently running sender/receiver's process ID.
bool ReadPreview(uint32_t processId, PreviewImage& image) noexcept;
}
