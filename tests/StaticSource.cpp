#include "common/FrameBus.h"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() try {
    lan3d::FrameBus bus;
    std::string error;
    if (!bus.Open(GetCurrentProcessId(), true, error)) throw std::runtime_error(error);
    lan3d::Frame frame{640, 360, 640 * 4, 1, lan3d::Timestamp100ns(), {}};
    frame.bgra.resize(size_t(frame.stride) * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
        auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + size_t(x) * 4;
        const bool right = x >= frame.width / 2;
        pixel[0] = right ? 193 : 37;
        pixel[1] = right ? 71 : 139;
        pixel[2] = right ? 43 : 217;
        pixel[3] = 255;
    }
    if (!bus.Publish(frame)) throw std::runtime_error("Cannot publish the single static SBS frame");
    bus.SetStatus("StaticSource: published exactly one frame; source is paused");
    std::cout << "StaticSource READY pid=" << GetCurrentProcessId()
        << " published=1 sequence=1 width=640 height=360\n" << std::flush;
    // Deliberately do not publish or modify the source timestamp again. Sender
    // refresh and reconnect must work without any further producer activity.
    std::this_thread::sleep_for(std::chrono::seconds(15));
    std::cout << "StaticSource stopped; published=1\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "StaticSource ERROR: " << e.what() << '\n';
    return 1;
}
