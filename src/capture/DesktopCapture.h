#pragma once
#include "common/Frame.h"
#include "common/GpuFrame.h"
#include <memory>
#include <string>

namespace lan3d {
enum class DesktopCaptureResult { FrameReady, NoChange, Error };
// monitorIndex follows EnumDisplayMonitors order (the receiver/UI order).
// Capture performs a zero-timeout acquisition and non-waiting staging/query poll.
// Pointer composition and rotated outputs are outside this first implementation.
class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;
    bool Open(unsigned monitorIndex, std::string& error);
    DesktopCaptureResult Capture(Frame& frame, std::string& error);
    // Returned texture is retained until the next CaptureGpu/RequestRefresh/Close.
    // The caller must finish GPU reads before any of those calls (NVENC sync completion).
    DesktopCaptureResult CaptureGpu(GpuFrame& frame, std::string& error);
    // Only a healthy, fully drained duplication can confirm that the cached
    // desktop is still current. Recovery/pending GPU copies cannot do so.
    bool CanRepeatCurrent() const;
    // Request a current snapshot even if no new desktop Present has occurred.
    // The next Capture uses the same non-waiting recovery/retry path.
    void RequestRefresh();
    void Close();
private:
    DesktopCaptureResult CaptureInternal(Frame* cpu, GpuFrame* gpu, std::string& error);
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
