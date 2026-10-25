#include "media/Codec.h"
#include "net/Transport.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace lan3d;
using namespace lan3d::media;
using namespace lan3d::net;
namespace {
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
Frame TestFrame(uint32_t w, uint32_t h, uint64_t index) {
    Frame f;
    f.width = w; f.height = h; f.stride = w * 4 + 32;
    f.sequence = 0x100000000ull + index;
    f.timestamp100ns = 0x200000000ull + (index % 3) * 123456; // duplicate/non-monotonic source time is intentional
    f.displayWidth = index % 3 ? w : 0; f.displayHeight = index % 3 ? h : 0;
    f.bgra.assign(size_t(f.stride) * h, 0xCD);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        auto* pixel = f.bgra.data() + size_t(y) * f.stride + x * 4;
        const bool left = x < w / 2, top = y < h / 2;
        const std::array<uint8_t, 3> color = top ? (left ? std::array<uint8_t, 3>{ 20, 30, 220 } : std::array<uint8_t, 3>{ 230, 60, 15 }) :
            (left ? std::array<uint8_t, 3>{ 255, 255, 255 } : std::array<uint8_t, 3>{ 0, 0, 0 });
        pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2]; pixel[3] = 255;
    }
    return f;
}
void CheckFrame(const Frame& decoded, const Frame& expected) {
    Require(ValidFrame(decoded), "Decoded frame failed validation");
    Require(decoded.width == expected.width && decoded.height == expected.height, "Visible dimensions were not preserved");
    Require(decoded.sequence == expected.sequence && decoded.timestamp100ns == expected.timestamp100ns, "Sequence/timestamp changed through H.264");
    Require(decoded.displayWidth == expected.displayWidth && decoded.displayHeight == expected.displayHeight,
        "Per-frame display aspect changed through codec/transport");
    for (uint32_t y : { expected.height / 4, expected.height * 3 / 4, expected.height - 2 }) {
        for (uint32_t x : { expected.width / 4, expected.width * 3 / 4 }) {
            const auto* a = decoded.bgra.data() + size_t(y) * decoded.stride + x * 4;
            const auto* b = expected.bgra.data() + size_t(y) * expected.stride + x * 4;
            for (unsigned channel = 0; channel < 3; ++channel) Require(std::abs(int(a[channel]) - int(b[channel])) <= 8, "BT.709 color/range or NV12 pitch/eye ordering mismatch");
            Require(a[3] == 255, "Decoded alpha is not opaque");
        }
    }
}
void Loopback(uint32_t width, uint32_t height, unsigned count) {
    std::string error;
    TcpServer server;
    Require(server.Listen("127.0.0.1", 0, error), error);
    std::exception_ptr threadError;
    unsigned receivedFrames{};
    std::thread receiver([&] {
        try {
            std::string localError;
            TcpConnection connection;
            Decoder decoder;
            Require(connection.Connect("127.0.0.1", server.port(), 3000, localError) == IoResult::Ok, localError);
            for (unsigned i = 0; i < count; ++i) {
                EncodedFrame packet;
                Require(connection.Receive(packet, 5000, localError) == IoResult::Ok, localError);
                Require(i != 0 || packet.keyframe, "First packet is not a keyframe");
                std::vector<Frame> frames;
                Require(decoder.Decode(packet, frames, localError), localError);
                for (const auto& frame : frames) {
                    CheckFrame(frame, TestFrame(width, height, receivedFrames));
                    ++receivedFrames;
                }
            }
        } catch (...) { threadError = std::current_exception(); }
    });
    try {
        TcpConnection connection;
        Require(server.Accept(connection, 3000, error) == IoResult::Ok, error);
        Encoder encoder;
        Require(encoder.Configure(width, height, 30, 12000000, error), error);
        uint64_t encodedBytes{};
        for (unsigned i = 0; i < count; ++i) {
            std::vector<EncodedFrame> packets;
            if (i == count / 2) Require(encoder.RequestKeyFrame(error), error);
            Require(encoder.Encode(TestFrame(width, height, i), packets, error), error);
            Require(packets.size() == 1, "Low latency encoder did not emit one packet per input");
            if (i == count / 2) Require(packets.front().keyframe, "Requested keyframe was not generated");
            for (const auto& packet : packets) {
                encodedBytes += packet.data.size();
                Require(connection.Send(packet, 3000, error) == IoResult::Ok, error);
            }
        }
        receiver.join();
        if (threadError) std::rethrow_exception(threadError);
        Require(receivedFrames == count, "Low latency decoder retained frames");
        std::cout << "H264/TCP loopback " << width << 'x' << height << ": " << receivedFrames << " frames, " << encodedBytes << " bytes, colors/stride/metadata/keyframes OK\n";
    } catch (...) {
        if (receiver.joinable()) receiver.join();
        throw;
    }
}
void Reconfigure() {
    Encoder encoder;
    Decoder decoder;
    std::string error;
    for (const auto dimensions : { std::array<uint32_t, 2>{ 640, 360 }, std::array<uint32_t, 2>{ 800, 601 } }) {
        Require(encoder.Configure(dimensions[0], dimensions[1], 30, 10000000, error), error);
        for (unsigned i = 0; i < 3; ++i) {
            const Frame input = TestFrame(dimensions[0], dimensions[1], i);
            std::vector<EncodedFrame> packets;
            std::vector<Frame> frames;
            Require(encoder.Encode(input, packets, error), error);
            Require(packets.size() == 1, "Resize encoder sample not emitted");
            Require(decoder.Decode(packets.front(), frames, error), error);
            Require(frames.size() == 1, "Resize decoder sample not emitted");
            CheckFrame(frames.front(), input);
        }
    }
    std::cout << "Resolution change and odd height OK\n";
}
void Put32(uint8_t* dst, uint32_t value) {
    dst[0] = uint8_t(value >> 24); dst[1] = uint8_t(value >> 16); dst[2] = uint8_t(value >> 8); dst[3] = uint8_t(value);
}
void Put64(uint8_t* dst, uint64_t value) { Put32(dst, uint32_t(value >> 32)); Put32(dst + 4, uint32_t(value)); }
void TransportBounds() {
    TcpServer server;
    TcpConnection incoming, outgoing;
    std::string error;
    Require(server.Listen("127.0.0.1", 0, error), error);
    Require(server.Accept(incoming, 20, error) == IoResult::Timeout, "Accept did not time out");
    Require(outgoing.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(incoming, 1000, error) == IoResult::Ok, error);
    EncodedFrame packet;
    Require(incoming.Receive(packet, 20, error) == IoResult::Timeout && incoming.IsOpen(), "Idle timeout lost framing");
    std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); incoming.Close(); });
    const auto start = std::chrono::steady_clock::now();
    const auto result = incoming.Receive(packet, 5000, error);
    cancel.join();
    Require(result == IoResult::Closed && std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Close did not promptly interrupt Receive");
    outgoing.Close();
    // Oversized announced payload must be rejected before allocation or reading it.
    for (bool partial : { false, true }) {
        SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(server.port()); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        Require(connect(raw, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "Raw test connect failed");
        Require(server.Accept(incoming, 1000, error) == IoResult::Ok, error);
        std::array<uint8_t, 80> h{};
        Put32(h.data(), 0x4c334453); Put32(h.data() + 4, 5); Put32(h.data() + 8, 80); Put32(h.data() + 12, 3);
        Put32(h.data() + 16, 640); Put32(h.data() + 20, 360); Put32(h.data() + 24, 30); Put32(h.data() + 28, 10000000);
        Put32(h.data() + 52, 0xffffffff); Put32(h.data() + 56, 0x48323634);
        Require(send(raw, reinterpret_cast<const char*>(h.data()), partial ? 10 : 80, 0) > 0, "Raw test send failed");
        Require(incoming.Receive(packet, 30, error) == IoResult::Error && !incoming.IsOpen() && packet.data.empty(), "Invalid/partial packet did not close connection");
        closesocket(raw);
    }
    // A FIN at the packet boundary is a clean stream end. The same FIN after
    // part of a header or payload must remain a protocol failure, not success.
    for (unsigned bytes : {0u, 10u, 81u}) {
        SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(server.port()); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        Require(connect(raw, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "EOF test connect failed");
        Require(server.Accept(incoming, 1000, error) == IoResult::Ok, error);
        std::array<uint8_t, 81> h{};
        Put32(h.data(), 0x4c334453); Put32(h.data() + 4, 5); Put32(h.data() + 8, 80); Put32(h.data() + 12, 3);
        Put32(h.data() + 16, 640); Put32(h.data() + 20, 360); Put32(h.data() + 24, 30); Put32(h.data() + 28, 10000000);
        Put32(h.data() + 52, 4); Put32(h.data() + 56, 0x48323634);
        if (bytes) Require(send(raw, reinterpret_cast<const char*>(h.data()), int(bytes), 0) == int(bytes), "EOF test send failed");
        Require(shutdown(raw, SD_SEND) == 0, "EOF test shutdown failed");
        const auto expected = bytes ? IoResult::Error : IoResult::Closed;
        Require(incoming.Receive(packet, 1000, error) == expected && !incoming.IsOpen() && packet.data.empty(), "Clean/truncated EOF classification mismatch");
        Require(bytes ? error.find("incomplete") != std::string::npos : error.empty(), "EOF diagnostic mismatch");
        closesocket(raw);
    }
    std::cout << "TCP idle/partial deadlines, cancellation, size validation and clean/truncated EOF OK\n";
}
void CodecNegotiation() {
    TcpServer server;
    std::string error;
    Require(server.Listen("127.0.0.1", 0, error), error);
    for (uint32_t mask : {0u, 1u, 3u, 7u}) {
        TcpConnection receiver, sender;
        Require(receiver.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
        Require(server.Accept(sender, 1000, error) == IoResult::Ok, error);
        Require(receiver.SendCapabilities(mask, 1000, error) == IoResult::Ok, error);
        uint32_t received = 99;
        Require(sender.ReceiveCapabilities(received, 1000, error) == IoResult::Ok && received == mask, "Capability mask did not round-trip");
        for (auto codec : {CodecId::H264, CodecId::Hevc, CodecId::Av1}) {
            EncodedFrame input;
            input.config = {640, 360, 60, 100000000, {1, 2, 3}, codec};
            input.sequence = 23; input.timestamp100ns = 0x200000000ull; input.keyframe = true;
            input.data = {4, 5, 6}; input.kind = ContentKind::Desktop;
            input.displayWidth = codec == CodecId::Av1 ? 0 : 3840; input.displayHeight = codec == CodecId::Av1 ? 0 : 2160;
            if (codec != CodecId::Av1) {
                input.audio.timestamp100ns = input.timestamp100ns - 1000000;
                input.audio.discontinuity = codec == CodecId::H264;
                input.audio.samples.resize(audio::MaxBlockFrames * audio::Channels);
                for (size_t i = 0; i < input.audio.samples.size(); ++i)
                    input.audio.samples[i] = i % 3 == 0 ? INT16_MIN : i % 3 == 1 ? INT16_MAX : -123;
            }
            Require(sender.Send(input, 1000, error) == IoResult::Ok, error);
            EncodedFrame output;
            Require(receiver.Receive(output, 1000, error) == IoResult::Ok, error);
            Require(output.config.codec == codec && output.data == input.data && output.config.sequenceHeader == input.config.sequenceHeader &&
                output.sequence == input.sequence && output.timestamp100ns == input.timestamp100ns && output.kind == input.kind &&
                output.displayWidth == input.displayWidth && output.displayHeight == input.displayHeight,
                "Video codec/content metadata did not round-trip");
            Require(output.audio.samples == input.audio.samples && output.audio.timestamp100ns == input.audio.timestamp100ns &&
                output.audio.discontinuity == input.audio.discontinuity, "PCM audio interval or signed samples did not round-trip");
        }
        Require(receiver.SendCapabilities(mask, 1000, error) == IoResult::Error && !receiver.IsOpen(), "Repeated capabilities were accepted");
    }
    // A peer cannot advertise unknown codecs or silently speak the old protocol.
    for (uint32_t version : {2u, 3u, 4u, 5u, 6u}) {
        SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(server.port());
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        Require(connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Capability raw connect failed");
        TcpConnection sender;
        Require(server.Accept(sender, 1000, error) == IoResult::Ok, error);
        std::array<uint8_t, 12> header{};
        Put32(header.data(), 0x4c334443); Put32(header.data() + 4, version); Put32(header.data() + 8, version == 5 ? 8 : 7);
        Require(send(raw, reinterpret_cast<const char*>(header.data()), int(header.size()), 0) == int(header.size()), "Capability raw send failed");
        uint32_t mask{};
        Require(sender.ReceiveCapabilities(mask, 1000, error) == IoResult::Error && !sender.IsOpen(), "Invalid capabilities were accepted");
        closesocket(raw);
    }
    std::cout << "Codec capability negotiation and H.264/HEVC/AV1 + max/empty PCM wire framing OK\n";
}
void IdleDisconnect() {
    TcpServer server;
    TcpConnection sender, receiver;
    std::string error;
    Require(server.Listen("127.0.0.1", 0, error), error);
    Require(receiver.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(sender, 1000, error) == IoResult::Ok, error);
    const auto before = std::chrono::steady_clock::now();
    for (unsigned n = 0; n < 20; ++n) Require(!sender.PollPeerClosed(), "Idle live peer was closed");
    Require(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "Idle peer check blocked");
    Require(receiver.SendCapabilities(7, 1000, error) == IoResult::Ok, error);
    Require(!sender.PollPeerClosed(), "Pending capability bytes were mistaken for EOF");
    uint32_t capabilities{};
    Require(sender.ReceiveCapabilities(capabilities, 1000, error) == IoResult::Ok && capabilities == 7,
        "Idle peer check consumed protocol data");
    receiver.Close();
    for (unsigned n = 0; n < 1000 && !sender.PollPeerClosed(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(!sender.IsOpen(), "Paused stream did not discover the receiver's FIN");
    // A replacement receiver can connect without a video frame from the source.
    Require(receiver.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(sender, 1000, error) == IoResult::Ok, error);
    Require(!sender.PollPeerClosed(), "Reconnected peer remained closed");
    receiver.Close(); sender.Close();
    SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(server.port());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    Require(connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Reset peer connect failed");
    Require(server.Accept(sender, 1000, error) == IoResult::Ok, error);
    linger reset{1, 0};
    Require(setsockopt(raw, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&reset), sizeof(reset)) == 0, "Reset peer linger failed");
    closesocket(raw);
    for (unsigned n = 0; n < 1000 && !sender.PollPeerClosed(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(!sender.IsOpen(), "Paused stream did not discover the receiver's reset");
    std::cout << "Idle peer peek: live socket, preserved protocol bytes, FIN/reset and paused-source reconnect OK\n";
}
void AudioBounds() {
    TcpServer server;
    TcpConnection incoming, outgoing;
    std::string error;
    Require(server.Listen("127.0.0.1", 0, error), error);
    auto connectRaw = [&] {
        SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(server.port());
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        Require(connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Raw audio test connect failed");
        Require(server.Accept(incoming, 1000, error) == IoResult::Ok, error);
        return raw;
    };
    auto header = [] {
        std::array<uint8_t, 80> h{};
        Put32(h.data(), 0x4c334453); Put32(h.data() + 4, 5); Put32(h.data() + 8, 80); Put32(h.data() + 12, 3);
        Put32(h.data() + 16, 640); Put32(h.data() + 20, 360); Put32(h.data() + 24, 30); Put32(h.data() + 28, 10000000);
        Put64(h.data() + 32, 23); Put64(h.data() + 40, 20000000);
        Put32(h.data() + 48, 3); Put32(h.data() + 52, 4); Put32(h.data() + 56, uint32_t(CodecId::H264));
        Put64(h.data() + 64, 19000000); Put32(h.data() + 72, 4); Put32(h.data() + 76, 1);
        return h;
    };
    {
        const SOCKET raw = connectRaw();
        const auto h = header();
        std::vector<uint8_t> bytes(h.begin(), h.end());
        bytes.insert(bytes.end(), {1, 2, 3, 4, 5, 6, 7}); // Sequence header and video.
        bytes.insert(bytes.end(), {0x00, 0x80, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x7F,
            0xC7, 0xCF, 0x39, 0x30, 0x00, 0xFF, 0x00, 0x01});
        Require(send(raw, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0) == int(bytes.size()), "Known PCM send failed");
        EncodedFrame packet;
        Require(incoming.Receive(packet, 1000, error) == IoResult::Ok, error);
        Require(packet.audio.samples == std::vector<int16_t>{INT16_MIN, -1, 0, INT16_MAX, -12345, 12345, -256, 256},
            "Wire PCM must be signed 16-bit little-endian, independently of header byte order");
        closesocket(raw);
    }
    for (unsigned bad = 0; bad < 14; ++bad) {
        const SOCKET raw = connectRaw();
        auto h = header();
        switch (bad) {
        case 0: Put32(h.data() + 72, audio::MaxBlockFrames + 1); break;
        case 1: Put32(h.data() + 76, 2); break;
        case 2: Put32(h.data() + 72, 0); Put32(h.data() + 76, 0); break; // Empty with timestamp.
        case 3: Put32(h.data() + 72, 0); Put64(h.data() + 64, 0); break; // Empty with discontinuity.
        case 4: Put64(h.data() + 64, 0); break;
        case 5: Put64(h.data() + 64, 20000001); break;
        case 6: Put64(h.data() + 64, 19999999); break; // Starts before video, ends after it.
        case 7: Put64(h.data() + 64, std::numeric_limits<uint64_t>::max() - 1); break;
        case 8: Put32(h.data() + 12, 2); break; // No initial keyframe.
        case 9: Put32(h.data() + 4, 3); Put32(h.data() + 8, 64); break;
        case 10: Put32(h.data() + 4, 4); break; // Prior version cannot describe half-SBS display aspect.
        case 11: Put32(h.data() + 60, 3840u << 16); break;
        case 12: Put32(h.data() + 60, 2160); break;
        case 13: Put32(h.data() + 60, ((MaxWidth + 1) << 16) | (MaxHeight + 1)); break;
        }
        const int headerBytes = bad == 9 ? 64 : bad == 10 ? 12 : int(h.size());
        Require(send(raw, reinterpret_cast<const char*>(h.data()), headerBytes, 0) == headerBytes, "Raw audio header send failed");
        EncodedFrame packet;
        Require(incoming.Receive(packet, 1000, error) == IoResult::Error && !incoming.IsOpen() &&
            packet.data.empty() && packet.audio.samples.empty() && !packet.audio.timestamp100ns &&
            !packet.displayWidth && !packet.displayHeight, "Malformed audio/header exposed a packet");
        Require(error.find(bad == 9 || bad == 10 ? "Incompatible" : "Rejected") != std::string::npos, "Malformed header was not rejected before waiting for payload");
        closesocket(raw);
    }
    for (const bool eof : {false, true}) {
        const SOCKET raw = connectRaw();
        auto h = header();
        std::vector<uint8_t> truncated(h.begin(), h.end());
        // A complete sequence header and video, followed by just half the PCM.
        truncated.resize(h.size() + 3 + 4 + 8, 0x42);
        Require(send(raw, reinterpret_cast<const char*>(truncated.data()), int(truncated.size()), 0) == int(truncated.size()), "Partial PCM send failed");
        if (eof) Require(shutdown(raw, SD_SEND) == 0, "Partial PCM shutdown failed");
        EncodedFrame packet;
        packet.audio.samples = {1, 2}; packet.data = {9};
        Require(incoming.Receive(packet, 30, error) == IoResult::Error && !incoming.IsOpen() &&
            packet.data.empty() && packet.audio.samples.empty() && !packet.audio.timestamp100ns && !packet.audio.discontinuity &&
            packet.config.sequenceHeader.empty() && !packet.timestamp100ns && !packet.sequence && !packet.keyframe,
            "Truncated PCM leaked complete video/audio from an incomplete media packet");
        Require(error.find("incomplete") != std::string::npos, "Truncated PCM was not classified as incomplete media");
        closesocket(raw);
    }
    Require(outgoing.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(incoming, 1000, error) == IoResult::Ok, error);
    for (unsigned bad = 0; bad < 9; ++bad) {
        EncodedFrame input;
        input.config = {640, 360, 30, 10000000, {}, CodecId::H264};
        input.sequence = 23; input.timestamp100ns = 20000000; input.keyframe = true; input.data = {4};
        input.audio.timestamp100ns = 19000000; input.audio.samples = {1, 2, 3, 4};
        switch (bad) {
        case 0: input.audio.samples.push_back(5); break;
        case 1: input.audio.samples.resize((audio::MaxBlockFrames + 1) * audio::Channels); break;
        case 2: input.audio.timestamp100ns = input.timestamp100ns; break;
        case 3: input.audio.timestamp100ns = 0; break;
        case 4: input.audio.samples.clear(); break;
        case 5: input.audio.timestamp100ns = std::numeric_limits<uint64_t>::max() - 1; break;
        case 6: input.displayWidth = 3840; break;
        case 7: input.displayWidth = MaxWidth + 1; input.displayHeight = 2160; break;
        case 8: input.displayHeight = 2160; break;
        }
        Require(outgoing.Send(input, 1000, error) == IoResult::Error, "Sender accepted invalid PCM/display aspect");
        EncodedFrame output;
        Require(incoming.Receive(output, 20, error) == IoResult::Timeout && incoming.IsOpen(), "Invalid outgoing PCM partially entered the wire");
    }
    std::cout << "PCM bounds/flags/timestamps, version rejection, partial-audio EOF/deadline and atomic packet delivery OK\n";
}
}
int main() {
    try {
        TransportBounds();
        IdleDisconnect();
        CodecNegotiation();
        AudioBounds();
        Loopback(640, 361, 8);
        Loopback(650, 359, 3);
        Reconfigure();
        Loopback(1920, 1080, 3);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
