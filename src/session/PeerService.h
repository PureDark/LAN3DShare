#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lan3d::session {
enum class State : uint8_t { Idle, Sending, Receiving, Busy };
struct Peer {
    std::string id, name, address;
    uint16_t controlPort{};
    State state{State::Idle};
};
struct DiscoveryTarget { std::string address; uint16_t port{}; };
struct Config {
    std::string localId; // Persistent canonical GUID supplied by the GUI.
    std::string displayName;
    std::string bindAddress{"0.0.0.0"};
    uint16_t discoveryPort{48731}, controlPort{};
    bool enableBroadcast{true};
    std::vector<DiscoveryTarget> testTargets;
    uint32_t helloIntervalMs{1000}, peerTimeoutMs{4000}, requestTimeoutMs{10000};
    uint32_t heartbeatIntervalMs{1000}, sessionTimeoutMs{4000};
};
struct Event {
    enum class Kind { IncomingRequest, SessionEnded };
    Kind kind{Kind::IncomingRequest};
    uint64_t requestId{}; // Local inbound request id; zero for an outbound session.
    Peer peer;
    std::string token;
    uint16_t mediaPort{}; // Sender's listening port; peer.address comes from TCP.
    std::string reason;
};
class Service {
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool Start(const Config& config, std::string& error);
    void Stop();
    std::vector<Peer> Snapshot() const;
    void SetState(State state);
    uint16_t localControlPort() const;
    uint16_t localDiscoveryPort() const;
    // Blocking handshake: call on a GUI background task, never the UI thread.
    // One token is exactly 32 hex digits and cannot be reused for 60 seconds.
    bool RequestReceive(const Peer& peer, const std::string& sessionToken,
        uint16_t mediaPort, uint32_t timeoutMs, std::string& error);
    // Respond after the local receiver has started, or reject with a reason.
    // false means expired/stopped/invalid; GUI should undo its preparation.
    bool Reply(uint64_t requestId, bool accepted, const std::string& reason, std::string& error);
    // Also cancels a matching pending outbound/inbound handshake.
    void EndSession(const Peer& peer, const std::string& token);
    std::vector<Event> PollEvents();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
