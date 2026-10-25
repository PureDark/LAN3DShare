#include "common/FrameBus.h"
#include "media/Codec.h"
#include "net/Transport.h"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace lan3d;
using namespace lan3d::net;
constexpr const char* Token = "0123456789abcdef0123456789abcdef";
void Require(bool value, const std::string& error) { if (!value) throw std::runtime_error(error); }
Frame MakeFrame(uint64_t sequence, ContentKind kind) {
    Frame frame{640, 360, 640 * 4, sequence, sequence * 333333, {}};
    frame.kind = kind;
    frame.bgra.resize(size_t(frame.stride) * frame.height, 127);
    for (size_t i = 3; i < frame.bgra.size(); i += 4) frame.bgra[i] = 255;
    return frame;
}
void KindBus() {
    FrameBus writer, reader;
    std::string error;
    Require(writer.Open(GetCurrentProcessId(), true, error), error);
    Require(reader.Open(GetCurrentProcessId(), false, error), error);
    uint64_t sequence{};
    for (const auto kind : {ContentKind::Desktop, ContentKind::LeiaSbs}) {
        const auto input = MakeFrame(sequence + 1, kind);
        Require(writer.Publish(input), "Content kind bus publish failed");
        Frame output;
        Require(reader.ReadLatest(output, sequence) && output.kind == kind, "FrameBus v2 lost content kind");
        sequence = output.sequence;
    }
}
void Session(bool correct) {
    std::string error;
    TcpServer server;
    Require(server.Listen("127.0.0.1", 0, error), error);
    TcpConnection client, accepted;
    Require(client.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(accepted, 1000, error) == IoResult::Ok, error);
    IoResult clientResult{};
    std::string clientError;
    std::thread handshake([&] {
        clientResult = client.AuthenticateClient(correct ? "0123456789ABCDEF0123456789ABCDEF" : "ffffffffffffffffffffffffffffffff", 1000, clientError);
    });
    const auto serverResult = accepted.AuthenticateServer(Token, 1000, error);
    handshake.join();
    if (!correct) {
        Require(serverResult == IoResult::Error && clientResult != IoResult::Ok, "Wrong session token was accepted");
        Require(!client.IsOpen() && !accepted.IsOpen(), "Rejected session did not close both connections");
        return;
    }
    Require(serverResult == IoResult::Ok && clientResult == IoResult::Ok, "Matching session token failed: " + error + clientError);
    media::Encoder encoder;
    media::Decoder decoder;
    Require(encoder.Configure(640, 360, 30, 2000000, error), error);
    uint64_t sequence{};
    for (const auto kind : {ContentKind::Desktop, ContentKind::LeiaSbs, ContentKind::Desktop}) {
        auto input = MakeFrame(++sequence, kind);
        input.displayWidth = kind == ContentKind::LeiaSbs ? 1280 : 640; input.displayHeight = 360;
        std::vector<media::EncodedFrame> packets;
        Require(encoder.Encode(input, packets, error) && packets.size() == 1, error);
        Require(packets.front().kind == kind, "Encoder metadata lost content kind");
        auto& audio = packets.front().audio;
        audio.timestamp100ns = input.timestamp100ns - 100000;
        audio.discontinuity = sequence == 1;
        audio.samples.resize(480 * 2);
        for (size_t i = 0; i < audio.samples.size(); ++i) audio.samples[i] = int16_t(int(i) * 61 - 30000);
        Require(accepted.Send(packets.front(), 1000, error) == IoResult::Ok, error);
        media::EncodedFrame packet;
        Require(client.Receive(packet, 1000, error) == IoResult::Ok && packet.kind == kind, "TCP v5 content flag mismatch: " + error);
        Require(packet.audio.samples == audio.samples && packet.audio.timestamp100ns == audio.timestamp100ns &&
            packet.audio.discontinuity == audio.discontinuity && packet.audio.End100ns() == packet.timestamp100ns,
            "Audio samples, timestamp, or discontinuity changed through the authenticated session");
        std::vector<Frame> output;
        Require(decoder.Decode(packet, output, error) && output.size() == 1, error);
        Require(output.front().kind == kind && output.front().sequence == sequence, "Decoder metadata lost kind or sequence");
        Require(output.front().displayWidth == input.displayWidth && output.front().displayHeight == input.displayHeight,
            "Content transition lost logical display aspect");
    }
}
void MissingPreface() {
    std::string error;
    TcpServer server;
    Require(server.Listen("127.0.0.1", 0, error), error);
    TcpConnection client, accepted;
    Require(client.Connect("127.0.0.1", server.port(), 1000, error) == IoResult::Ok, error);
    Require(server.Accept(accepted, 1000, error) == IoResult::Ok, error);
    const auto start = std::chrono::steady_clock::now();
    Require(accepted.AuthenticateServer(Token, 40, error) == IoResult::Timeout && !accepted.IsOpen(), "Missing preface did not close on deadline");
    Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Handshake exceeded its total deadline");
}
}
int main() try {
    Require(ValidSessionToken(Token), "Valid token rejected");
    Require(!ValidSessionToken("") && !ValidSessionToken("not-a-token") && !ValidSessionToken("0123456789abcdef0123456789abcdeg"), "Malformed token accepted");
    KindBus();
    Session(true);
    Session(false);
    MissingPreface();
    std::cout << "Session transport PASS: matching/wrong/missing tokens; FrameBus + H264 + TCP v5 Desktop/SBS/audio/aspect transitions\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "Session transport FAILED: " << e.what() << '\n'; return 1; }
