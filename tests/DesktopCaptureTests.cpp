#include "capture/DesktopCapture.h"
#include <Windows.h>
#include <iostream>
int main() {
    lan3d::DesktopCapture capture; std::string error;
    if (capture.CanRepeatCurrent()) { std::cerr << "closed capture granted audio timeline credit\n"; return 1; }
    if (capture.Open(~0u, error)) { std::cerr << "invalid monitor accepted\n"; return 1; }
    if (!capture.Open(0, error)) { std::cout << "SKIP: desktop duplication is unavailable in this session: " << error << '\n'; return 77; }
    if (capture.CanRepeatCurrent()) { std::cerr << "uncaptured desktop granted audio timeline credit\n"; return 1; }
    lan3d::Frame frame;
    for (unsigned i = 0; i < 200; ++i) {
        const auto result = capture.Capture(frame, error);
        if (result == lan3d::DesktopCaptureResult::Error) { std::cerr << error << '\n'; return 1; }
        if (result == lan3d::DesktopCaptureResult::FrameReady) {
            if (!lan3d::ValidFrame(frame) || frame.kind != lan3d::ContentKind::Desktop || !frame.sequence || !frame.timestamp100ns) {
                std::cerr << "invalid desktop frame metadata\n"; return 1;
            }
            // Read-only validation: no screenshot file, display changes, window
            // movement, input simulation or application injection.
            std::cout << "Desktop duplication read-only capture: " << frame.width << 'x' << frame.height << " BGRA8, Desktop metadata PASS\n";
            const auto sequence = frame.sequence;
            capture.RequestRefresh(); bool refreshed{};
            if (capture.CanRepeatCurrent()) { std::cerr << "refreshing desktop granted stale audio timeline credit\n"; return 1; }
            for (unsigned retry = 0; retry < 200; ++retry) {
                const auto next = capture.Capture(frame, error);
                if (next == lan3d::DesktopCaptureResult::Error) { std::cerr << error << '\n'; return 1; }
                if (next == lan3d::DesktopCaptureResult::FrameReady) {
                    refreshed = lan3d::ValidFrame(frame) && frame.kind == lan3d::ContentKind::Desktop && frame.sequence > sequence;
                    break;
                }
                Sleep(5);
            }
            if (!refreshed) { std::cerr << "explicit desktop snapshot refresh failed: " << error << '\n'; return 1; }
            capture.Close();
            if (capture.CanRepeatCurrent()) { std::cerr << "closed desktop retained audio timeline credit\n"; return 1; }
            if (capture.Capture(frame, error) != lan3d::DesktopCaptureResult::Error) return 1;
            return 0;
        }
        Sleep(5);
    }
    std::cerr << "desktop frame timeout: " << error << '\n'; return 1;
}
