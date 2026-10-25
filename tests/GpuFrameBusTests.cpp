#include "common/GpuFrameBus.h"
#include <iostream>
#include <stdexcept>
using namespace lan3d;
void Require(bool value) { if (!value) throw std::runtime_error("GPU mailbox ownership check failed"); }
int main() try {
    GpuFrameBus producer, consumer, other;
    std::string error;
    const auto pid = GetCurrentProcessId();
    Require(producer.Open(pid, true, error));
    Require(consumer.Open(pid, false, error));
    Require(other.Open(pid, false, error));
    GpuFrame frame; frame.width = 3840; frame.height = 2160;
    frame.format = DXGI_FORMAT_B8G8R8A8_UNORM; frame.sequence = frame.timestamp100ns = 1;
    wcscpy_s(frame.textureName, L"Local\\LAN3DShare.test.surface");
    Require(producer.Publish(frame));
    GpuFrame first; Require(consumer.AcquireLatest(first, 0));
    Require(!producer.IsReleased(1)); Require(!other.Release(1));
    // Latest unclaimed work is replaceable, but the active GPU read is not.
    frame.sequence = 2; Require(producer.Publish(frame));
    frame.sequence = 3; Require(producer.Publish(frame));
    Require(producer.IsReleased(2)); Require(!producer.IsReleased(1));
    GpuFrame second; Require(other.AcquireLatest(second, 0)); Require(second.sequence == 3);
    Require(!consumer.AcquireLatest(second, 0));
    Require(consumer.Release(1)); Require(producer.IsReleased(1));
    Require(other.Release(3)); Require(producer.IsReleased(3));
    // A disconnected consumer cannot transfer an uncompleted lease to a new owner.
    frame.sequence = 4; Require(producer.Publish(frame));
    { GpuFrameBus abandoned; Require(abandoned.Open(pid, false, error)); Require(abandoned.AcquireLatest(first, 0)); }
    Require(!producer.IsReleased(4)); Require(!consumer.Release(4));
    frame.sequence = 5; Require(producer.Publish(frame));
    Require(consumer.AcquireLatest(first, 4)); Require(first.sequence == 5);
    Require(consumer.Release(5)); Require(!producer.IsReleased(4));
    Require(!producer.Publish(frame));
    // Explicit disconnect has the same quarantine contract as destruction.
    frame.sequence = 6; Require(producer.Publish(frame));
    Require(consumer.AcquireLatest(first, 5)); Require(first.sequence == 6);
    consumer.Close(); consumer.Close();
    Require(!consumer.AcquireLatest(first, 0)); Require(!consumer.Release(6));
    Require(!consumer.Publish(frame)); Require(!consumer.IsReleased(6));
    Require(!producer.IsReleased(6)); Require(!other.Release(6));
    Require(consumer.Open(pid, false, error));
    Require(!consumer.Release(6)); Require(!consumer.AcquireLatest(first, 0));
    frame.sequence = 7; Require(producer.Publish(frame));
    Require(consumer.AcquireLatest(first, 0)); Require(first.sequence == 7);
    Require(consumer.Release(7)); Require(!producer.IsReleased(4)); Require(!producer.IsReleased(6));
    // Once all handles to the old PID generation close, its named mapping
    // disappears and a replacement producer may start its sequence at one.
    producer.Close(); consumer.Close(); other.Close();
    Require(!other.Open(pid, false, error));
    Require(producer.Open(pid, true, error));
    Require(consumer.Open(pid, false, error));
    Require(!consumer.AcquireLatest(first, 0));
    frame.sequence = 1; Require(producer.Publish(frame));
    Require(consumer.AcquireLatest(first, 0)); Require(first.sequence == 1);
    Require(consumer.Release(1)); Require(producer.IsReleased(1));
    std::cout << "GPU mailbox latest-frame selection, lease ownership and closed-generation reset passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
