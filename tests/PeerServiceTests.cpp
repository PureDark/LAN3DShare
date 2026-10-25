#include "session/PeerService.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace lan3d::session;
using Clock = std::chrono::steady_clock;
namespace {
void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
constexpr const char* IdA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr const char* IdB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr const char* IdC = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
std::string Token(unsigned value) { auto text = std::to_string(value); return std::string(32 - text.size(), '0') + text; }
Config Configuration(const char* id, const char* name) {
    Config c; c.localId = id; c.displayName = name; c.bindAddress = "127.0.0.1";
    c.discoveryPort = 0; c.enableBroadcast = false;
    c.helloIntervalMs = 40; c.peerTimeoutMs = 200; c.requestTimeoutMs = 600;
    c.heartbeatIntervalMs = 50; c.sessionTimeoutMs = 250; return c;
}
template<class F> bool Until(F condition, unsigned milliseconds = 1500) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (Clock::now() < deadline) { if (condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return condition();
}
Event WaitEvent(Service& service, Event::Kind kind, unsigned milliseconds = 1500) {
    Event event; bool found{};
    Check(Until([&] { for (auto& next : service.PollEvents()) if (next.kind == kind) { event = std::move(next); found = true; } return found; }, milliseconds), "Expected peer service event was not delivered");
    return event;
}
Peer Find(Service& service, const char* id) {
    Peer result;
    Check(Until([&] { for (auto& peer : service.Snapshot()) if (peer.id == id) { result = peer; return true; } return false; }), "Peer was not discovered");
    return result;
}
struct Result { bool accepted{}; std::string error; };
auto Request(Service& service, const Peer& peer, unsigned token, unsigned timeout = 1200) {
    return std::async(std::launch::async, [&service, peer, token, timeout] { Result r; r.accepted = service.RequestReceive(peer, Token(token), 48730, timeout, r.error); return r; });
}
void Accept(Service& receiver) {
    const auto event = WaitEvent(receiver, Event::Kind::IncomingRequest);
    Check(event.peer.address == "127.0.0.1" && event.mediaPort == 48730, "Request did not preserve TCP source IP or media port");
    std::string error; Check(receiver.Reply(event.requestId, true, {}, error), error);
}
struct RawSocket {
    SOCKET socket{INVALID_SOCKET};
    ~RawSocket() { if (socket != INVALID_SOCKET) closesocket(socket); }
};
sockaddr_in Loopback(uint16_t port) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr); return a; }
void Put16(std::vector<uint8_t>& b, unsigned n) { b.push_back(uint8_t(n >> 8)); b.push_back(uint8_t(n)); }
void Put32(std::vector<uint8_t>& b, uint32_t n) { Put16(b, n >> 16); Put16(b, n); }
void Append(std::vector<uint8_t>& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
std::vector<uint8_t> Wire(unsigned type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> b; Put32(b, 0x4c335053); Put16(b, 1); Put16(b, type); Put32(b, static_cast<uint32_t>(body.size())); Put32(b, 0); b.insert(b.end(), body.begin(), body.end()); return b;
}
void SendBytes(SOCKET s, const std::vector<uint8_t>& bytes) {
    size_t at{}; while (at < bytes.size()) { const int n = send(s, reinterpret_cast<const char*>(bytes.data() + at), static_cast<int>(bytes.size() - at), 0); Check(n > 0, "Raw loopback send failed"); at += n; }
}
void ReceiveBytes(SOCKET s, uint8_t* bytes, size_t size) {
    size_t at{}; while (at < size) { const int n = recv(s, reinterpret_cast<char*>(bytes + at), static_cast<int>(size - at), 0); Check(n > 0, "Raw loopback receive failed"); at += n; }
}
void Malformed(Service& service) {
    const auto peerCount = service.Snapshot().size();
    RawSocket udp{socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    auto address = Loopback(service.localDiscoveryPort()); const std::array<char, 8> bad{};
    Check(sendto(udp.socket, bad.data(), static_cast<int>(bad.size()), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == bad.size(), "Malformed UDP test send failed");
    for (bool silent : {false, true}) {
        RawSocket raw{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        address = Loopback(service.localControlPort());
        Check(connect(raw.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Malformed test connect failed");
        const DWORD timeout = 1500; setsockopt(raw.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (!silent) { auto header = Wire(2, {}); header[8] = 0x7F; SendBytes(raw.socket, header); }
        char byte{}; Check(recv(raw.socket, &byte, 1, 0) == 0, "Invalid or silent control connection was not closed within deadline");
    }
    Check(service.PollEvents().empty(), "Invalid control packet reached the GUI");
    Check(service.Snapshot().size() == peerCount, "Invalid discovery packet changed the peer list");
}
void HeartbeatTimeout(Service& sender) {
    RawSocket listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    auto address = Loopback(0);
    Check(bind(listener.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener.socket, 1) == 0, "Raw heartbeat listener failed");
    int length = sizeof(address); Check(getsockname(listener.socket, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Raw heartbeat port failed");
    Peer peer{IdC, "Silent test peer", "127.0.0.1", ntohs(address.sin_port), State::Idle};
    auto server = std::async(std::launch::async, [&] {
        RawSocket connection{accept(listener.socket, nullptr, nullptr)};
        Check(connection.socket != INVALID_SOCKET, "Raw heartbeat accept failed");
        const DWORD timeout = 1500; setsockopt(connection.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::array<uint8_t, 16> header{}; ReceiveBytes(connection.socket, header.data(), header.size());
        const size_t size = (size_t(header[8]) << 24) | (size_t(header[9]) << 16) | (size_t(header[10]) << 8) | header[11];
        Check(size <= 512, "Unexpected request length"); std::vector<uint8_t> body(size); ReceiveBytes(connection.socket, body.data(), body.size());
        std::vector<uint8_t> response; Append(response, IdC); Append(response, Token(20)); Put16(response, 1); Put16(response, 0);
        SendBytes(connection.socket, Wire(3, response));
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    });
    std::string error; Check(sender.RequestReceive(peer, Token(20), 48730, 1000, error), error);
    const auto event = WaitEvent(sender, Event::Kind::SessionEnded);
    Check(event.reason.find("heartbeat") != std::string::npos && event.token == Token(20), "Missing-heartbeat session did not time out");
    server.get();
}
}
int main() try {
    Service a, b, c; std::string error;
    auto configA = Configuration(IdA, "Client A"); Check(a.Start(configA, error), error);
    auto configB = Configuration(IdB, "Client B"); configB.testTargets.push_back({"127.0.0.1", a.localDiscoveryPort()}); Check(b.Start(configB, error), error);
    auto peerB = Find(a, IdB); Check(peerB.name == "Client B" && peerB.address == "127.0.0.1" && peerB.controlPort == b.localControlPort(), "Discovery identity/address/port mismatch");
    b.SetState(State::Busy); Check(Until([&] { const auto peers = a.Snapshot(); return !peers.empty() && peers.front().state == State::Busy; }), "Discovery state update missing");
    b.Stop(); Check(Until([&] { return a.Snapshot().empty(); }), "Offline peer did not expire");
    configB.displayName = "Client B renamed"; Check(b.Start(configB, error), error); peerB = Find(a, IdB); Check(peerB.name == configB.displayName, "Reappearing peer name did not update");
    Peer peerA{IdA, "Client A", "127.0.0.1", a.localControlPort(), State::Idle};
    auto accepted = Request(a, peerB, 1); Accept(b); const auto acceptedResult = accepted.get(); Check(acceptedResult.accepted, acceptedResult.error);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); Check(a.PollEvents().empty() && b.PollEvents().empty(), "Healthy session lost its heartbeat");
    Check(!a.RequestReceive(peerB, Token(1), 48730, 300, error), "Duplicate outbound session was accepted");
    Check(c.Start(Configuration(IdC, "Client C"), error), error); Check(!c.RequestReceive(peerB, Token(2), 48730, 500, error) && error.find("busy") != std::string::npos, "Busy receiver accepted a second client");
    a.EndSession(peerB, Token(999)); std::this_thread::sleep_for(std::chrono::milliseconds(70)); Check(a.PollEvents().empty(), "Wrong token ended a session");
    a.EndSession(peerB, Token(1)); Check(WaitEvent(a, Event::Kind::SessionEnded).token == Token(1), "Local end token mismatch"); Check(WaitEvent(b, Event::Kind::SessionEnded).token == Token(1), "Remote end token mismatch");
    Check(!a.RequestReceive(peerB, Token(1), 48730, 300, error), "Expired session token was reused");
    auto rejected = Request(a, peerB, 3); auto incoming = WaitEvent(b, Event::Kind::IncomingRequest); Check(b.Reply(incoming.requestId, false, "User is busy", error), error); const auto rejectResult = rejected.get(); Check(!rejectResult.accepted && rejectResult.error == "User is busy", "GUI rejection reason was not propagated");
    auto timedOut = Request(a, peerB, 4, 350); incoming = WaitEvent(b, Event::Kind::IncomingRequest); Check(!timedOut.get().accepted, "Unanswered request did not time out"); Check(WaitEvent(b, Event::Kind::SessionEnded).requestId == incoming.requestId, "Expired request cleanup event missing"); Check(!b.Reply(incoming.requestId, true, {}, error), "Late GUI acceptance was allowed");
    auto togetherA = Request(a, peerB, 5, 350); auto togetherB = Request(b, peerA, 6, 350);
    Check(!togetherA.get().accepted && !togetherB.get().accepted, "Simultaneous requests bypassed exclusive session arbitration");
    Until([&] { a.PollEvents(); b.PollEvents(); return true; }); std::this_thread::sleep_for(std::chrono::milliseconds(100)); a.PollEvents(); b.PollEvents();
    HeartbeatTimeout(a);
    Malformed(b);
    auto cancelled = Request(a, peerB, 8); incoming = WaitEvent(b, Event::Kind::IncomingRequest);
    const auto cancelStart = Clock::now(); a.EndSession(peerB, Token(8)); Check(!cancelled.get().accepted, "Pending outbound cancellation was ignored");
    Check(WaitEvent(b, Event::Kind::SessionEnded).requestId == incoming.requestId && Clock::now() - cancelStart < std::chrono::milliseconds(500), "Remote GUI was not notified of a cancelled pending request");
    auto cancelledInbound = Request(a, peerB, 9); incoming = WaitEvent(b, Event::Kind::IncomingRequest); b.EndSession(incoming.peer, incoming.token);
    Check(!cancelledInbound.get().accepted, "Pending inbound cancellation was ignored");
    auto stopping = Request(a, peerB, 7, 2000); incoming = WaitEvent(b, Event::Kind::IncomingRequest);
    const auto stopStart = Clock::now(); a.Stop(); Check(!stopping.get().accepted && Clock::now() - stopStart < std::chrono::seconds(1), "Stop failed to interrupt an outbound request");
    b.Stop(); c.Stop();
    std::cout << "PASS loopback-only discovery/state/expiry, accept/reject/busy/end, tokens, simultaneous requests, request/heartbeat timeouts, malformed messages and Stop\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
