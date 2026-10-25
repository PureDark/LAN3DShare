#include "session/PeerService.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace lan3d::session {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t Magic = 0x4c335053; // L3PS
constexpr size_t HeaderBytes = 16, MaxPayload = 512, MaxPeers = 128, MaxWorkers = 16, MaxTokens = 1024;
enum class Type : uint16_t { Hello = 1, Request, Response, Heartbeat, End };
struct Socket {
    SOCKET value{INVALID_SOCKET};
    explicit Socket(SOCKET s = INVALID_SOCKET) : value(s) {}
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
using SocketPtr = std::shared_ptr<Socket>;
struct Winsock {
    int result;
    Winsock() { WSADATA data{}; result = WSAStartup(MAKEWORD(2, 2), &data); }
    ~Winsock() { if (!result) WSACleanup(); }
};
bool NetworkReady(std::string& error) {
    static Winsock ws;
    if (!ws.result) return true;
    error = "WSAStartup failed: " + std::to_string(ws.result); return false;
}
bool Fail(std::string& error, const char* operation, int code = WSAGetLastError()) {
    error = std::string(operation) + " failed (Winsock " + std::to_string(code) + ")"; return false;
}
bool Hex(const std::string& text, size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool Guid(const std::string& text) {
    if (text.size() != 36) return false;
    std::string compact;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else compact += text[i];
    }
    return Hex(compact, 32);
}
std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
    return value;
}
bool Text(const std::string& text, size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && text.empty()) || text.size() > maximum) return false;
    for (unsigned char c : text) if (c < 32 || c == 127) return false;
    return text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}
bool Address(const std::string& ip, uint16_t port, sockaddr_in& address) {
    address = {}; address.sin_family = AF_INET; address.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &address.sin_addr) == 1;
}
std::string Ip(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) ? text : "";
}
bool Nonblocking(SOCKET socket, bool tcp, std::string& error) {
    u_long one = 1;
    if (ioctlsocket(socket, FIONBIO, &one) != 0) return Fail(error, "nonblocking socket");
    if (tcp) { const BOOL yes = TRUE; if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes))) return Fail(error, "TCP_NODELAY"); }
    return true;
}
void U16(Bytes& out, uint16_t n) { out.push_back(uint8_t(n >> 8)); out.push_back(uint8_t(n)); }
void U32(Bytes& out, uint32_t n) { U16(out, uint16_t(n >> 16)); U16(out, uint16_t(n)); }
void Fixed(Bytes& out, const std::string& text) { out.insert(out.end(), text.begin(), text.end()); }
void String(Bytes& out, const std::string& text) { U16(out, static_cast<uint16_t>(text.size())); Fixed(out, text); }
struct Reader {
    const Bytes& bytes; size_t at{}; bool ok{true};
    uint16_t U16() { if (at + 2 > bytes.size()) { ok = false; return 0; } const auto n = uint16_t((uint16_t(bytes[at]) << 8) | bytes[at + 1]); at += 2; return n; }
    uint32_t U32() { const auto hi = U16(); return (uint32_t(hi) << 16) | U16(); }
    std::string Fixed(size_t size) { if (size > bytes.size() - at) { ok = false; return {}; } std::string s(bytes.begin() + at, bytes.begin() + at + size); at += size; return s; }
    std::string String() { return Fixed(U16()); }
    bool End() const { return ok && at == bytes.size(); }
};
Bytes Message(Type type, const Bytes& payload) {
    Bytes wire; wire.reserve(HeaderBytes + payload.size());
    U32(wire, Magic); U16(wire, 1); U16(wire, static_cast<uint16_t>(type)); U32(wire, static_cast<uint32_t>(payload.size())); U32(wire, 0);
    wire.insert(wire.end(), payload.begin(), payload.end()); return wire;
}
bool Header(const Bytes& header, Type& type, uint32_t& size) {
    Reader r{header}; if (r.U32() != Magic || r.U16() != 1) return false;
    const auto raw = r.U16(); size = r.U32();
    if (raw < 1 || raw > 5 || size > MaxPayload || r.U32() != 0 || !r.End()) return false;
    type = static_cast<Type>(raw); return true;
}
enum class Io { Ok, Timeout, Closed, Stopped, Error };
Io Wait(SOCKET socket, bool writing, Time deadline, const std::function<bool()>& stopped) {
    for (;;) {
        if (stopped()) return Io::Stopped;
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return Io::Timeout;
        timeval timeout{}; timeout.tv_usec = static_cast<long>(std::min<int64_t>(remaining, 50000));
        fd_set ready, errors; FD_ZERO(&ready); FD_ZERO(&errors); FD_SET(socket, &ready); FD_SET(socket, &errors);
        const int count = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout);
        if (count == SOCKET_ERROR || FD_ISSET(socket, &errors)) return Io::Error;
        if (count && FD_ISSET(socket, &ready)) return Io::Ok;
    }
}
Io Transfer(SOCKET socket, bool writing, uint8_t* data, size_t size, Time deadline,
    const std::function<bool()>& stopped, size_t& transferred) {
    size_t offset = 0;
    while (offset < size) {
        const auto ready = Wait(socket, writing, deadline, stopped); if (ready != Io::Ok) return ready;
        const auto count = writing ? send(socket, reinterpret_cast<const char*>(data + offset), static_cast<int>(size - offset), 0) :
            recv(socket, reinterpret_cast<char*>(data + offset), static_cast<int>(size - offset), 0);
        if (!count) return Io::Closed;
        if (count == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) continue; return Io::Error; }
        offset += count; transferred += count;
    }
    return Io::Ok;
}
bool Send(SOCKET socket, Type type, const Bytes& payload, Time deadline, const std::function<bool()>& stopped) {
    auto wire = Message(type, payload); size_t total{};
    return Transfer(socket, true, wire.data(), wire.size(), deadline, stopped, total) == Io::Ok;
}
Io Receive(SOCKET socket, Type& type, Bytes& payload, Time deadline, const std::function<bool()>& stopped, Time packetDeadline = {}) {
    if (packetDeadline != Time{}) {
        const auto ready = Wait(socket, false, deadline, stopped);
        if (ready != Io::Ok) return ready;
        // The next heartbeat is an idle poll deadline, not a deadline for a
        // fragmented control message that has already started arriving.
        deadline = packetDeadline;
    }
    Bytes header(HeaderBytes); size_t total{};
    auto result = Transfer(socket, false, header.data(), header.size(), deadline, stopped, total);
    if (result != Io::Ok) return result == Io::Timeout && total ? Io::Error : result;
    uint32_t size{}; if (!Header(header, type, size)) return Io::Error;
    payload.resize(size);
    result = Transfer(socket, false, payload.data(), payload.size(), deadline, stopped, total);
    return result == Io::Timeout ? Io::Error : result;
}
SocketPtr Connect(const Peer& peer, Time deadline, const std::function<bool()>& stopped, std::string& error) {
    sockaddr_in address{};
    if (!peer.controlPort || !Address(peer.address, peer.controlPort, address)) { error = "Invalid peer IPv4 address or port"; return {}; }
    auto socket = std::make_shared<Socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket->value == INVALID_SOCKET || !Nonblocking(socket->value, true, error)) { if (error.empty()) Fail(error, "control socket"); return {}; }
    if (connect(socket->value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { Fail(error, "control connect"); return {}; }
        if (Wait(socket->value, true, deadline, stopped) != Io::Ok) { error = "Control connection timed out, stopped, or was refused"; return {}; }
        int result{}; int bytes = sizeof(result);
        if (getsockopt(socket->value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&result), &bytes) || result) { Fail(error, "control connect", result); return {}; }
    }
    return socket;
}
std::vector<DiscoveryTarget> BroadcastTargets(uint16_t port) {
    std::set<std::string> addresses{"255.255.255.255"};
    ULONG size = 16384; std::vector<uint8_t> buffer(size);
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW && size <= 1024 * 1024) {
        buffer.resize(size); result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == NO_ERROR) for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET || !unicast->OnLinkPrefixLength || unicast->OnLinkPrefixLength >= 32) continue;
            sockaddr_in target = *reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            const auto mask = UINT32_MAX << (32 - unicast->OnLinkPrefixLength);
            target.sin_addr.s_addr = htonl((ntohl(target.sin_addr.s_addr) & mask) | ~mask);
            addresses.insert(Ip(target));
        }
    }
    std::vector<DiscoveryTarget> targets; for (const auto& ip : addresses) targets.push_back({ip, port}); return targets;
}
}

struct Service::Impl {
    struct Seen { Peer peer; Time time; };
    struct Pending {
        uint64_t id{}; Peer peer; std::string token; uint16_t mediaPort{};
        Time deadline; int decision{-1}; std::string reason;
    };
    struct Session {
        Peer peer; std::string token; uint64_t requestId{}; uint16_t mediaPort{};
        SocketPtr socket; std::atomic<bool> end{false};
    };
    struct Worker { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    Config config;
    mutable std::mutex mutex;
    std::mutex lifecycle, workersMutex;
    std::condition_variable changed;
    std::atomic<bool> stopping{true};
    std::atomic<uint16_t> controlPort{}, discoveryPort{};
    State state{State::Idle};
    SocketPtr udp, listener;
    std::thread main;
    std::vector<Worker> workers;
    std::map<std::string, Seen> peers;
    std::map<std::string, Time> recentTokens;
    std::vector<Event> events;
    std::shared_ptr<Pending> pending;
    std::shared_ptr<Session> session;
    std::string outbound, outboundPeer;
    std::shared_ptr<std::atomic<bool>> outboundCancel;
    uint64_t nextRequest{1};
    unsigned externalRequests{};
    void Emit(Event event) { if (events.size() >= 64) events.erase(events.begin()); events.push_back(std::move(event)); }
    bool Stopped() const { return stopping.load(); }
    bool Spawn(std::function<void()> operation) {
        std::lock_guard lock(workersMutex);
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->done->load()) { it->thread.join(); it = workers.erase(it); } else ++it;
        }
        if (Stopped() || workers.size() >= MaxWorkers) return false;
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.push_back({std::thread([operation = std::move(operation), done] { try { operation(); } catch (...) {} done->store(true); }), done});
        return true;
    }
    Bytes Hello() {
        std::lock_guard lock(mutex);
        Bytes b; Fixed(b, config.localId); U16(b, controlPort); U16(b, static_cast<uint16_t>(state)); String(b, config.displayName); return b;
    }
    Bytes Response(const std::string& token, bool accepted, const std::string& reason) const {
        Bytes b; Fixed(b, config.localId); Fixed(b, token); U16(b, accepted ? 1 : 0); String(b, reason); return b;
    }
    Bytes Identity(const std::string& token) const { Bytes b; Fixed(b, config.localId); Fixed(b, token); return b; }
    void Finish(const std::shared_ptr<Session>& ended, const std::string& reason) {
        std::lock_guard lock(mutex);
        if (session != ended) return;
        Emit({Event::Kind::SessionEnded, ended->requestId, ended->peer, ended->token, ended->mediaPort, reason});
        session.reset(); state = State::Idle;
    }
    void SessionLoop(const std::shared_ptr<Session>& current) {
        auto nextHeartbeat = Clock::now(); auto lastReceived = Clock::now();
        std::string reason = "Session ended locally"; bool remoteEnded = false;
        const auto cancelled = [&] { return Stopped() || current->end.load(); };
        while (!cancelled()) {
            const auto now = Clock::now();
            if (now - lastReceived >= std::chrono::milliseconds(config.sessionTimeoutMs)) { reason = "Session heartbeat timed out"; break; }
            if (now >= nextHeartbeat) {
                if (!Send(current->socket->value, Type::Heartbeat, Identity(current->token), now + std::chrono::milliseconds(500), cancelled)) { reason = "Session connection closed"; break; }
                nextHeartbeat = now + std::chrono::milliseconds(config.heartbeatIntervalMs);
            }
            Type type{}; Bytes body;
            const auto result = Receive(current->socket->value, type, body, std::min(nextHeartbeat, lastReceived + std::chrono::milliseconds(config.sessionTimeoutMs)), cancelled,
                lastReceived + std::chrono::milliseconds(config.sessionTimeoutMs));
            if (result == Io::Timeout) continue;
            if (result == Io::Stopped) break;
            if (result != Io::Ok) { reason = "Session connection closed or sent an invalid packet"; break; }
            Reader r{body}; const auto id = Lower(r.Fixed(36)), token = Lower(r.Fixed(32));
            if (id != current->peer.id || token != current->token) { reason = "Session identity mismatch"; break; }
            if (type == Type::Heartbeat && r.End()) { lastReceived = Clock::now(); continue; }
            if (type == Type::End) {
                const auto text = r.String();
                if (r.End() && Text(text, 192, true)) { reason = text.empty() ? "Peer ended the session" : text; remoteEnded = true; break; }
            }
            reason = "Invalid session control message"; break;
        }
        if (!remoteEnded) {
            auto body = Identity(current->token); String(body, reason);
            Send(current->socket->value, Type::End, body, Clock::now() + std::chrono::milliseconds(150), [] { return false; });
        }
        Finish(current, reason);
    }
    void HandleRequest(const SocketPtr& socket, const std::string& address) {
        Type type{}; Bytes body;
        const auto stopped = [&] { return Stopped(); };
        if (Receive(socket->value, type, body, Clock::now() + std::chrono::milliseconds(config.requestTimeoutMs), stopped) != Io::Ok || type != Type::Request) return;
        Reader r{body};
        Peer peer; peer.id = Lower(r.Fixed(36)); const auto target = Lower(r.Fixed(36)); const auto token = Lower(r.Fixed(32));
        const auto mediaPort = r.U16(); peer.controlPort = r.U16(); const auto requestedWait = r.U32(); peer.name = r.String(); peer.address = address;
        if (!r.End() || !Guid(peer.id) || peer.id == config.localId || target != config.localId || !Hex(token, 32) ||
            !mediaPort || !peer.controlPort || !Text(peer.name, 96) || requestedWait < 100 || requestedWait > 30000) return;
        auto request = std::make_shared<Pending>();
        request->peer = peer; request->token = token; request->mediaPort = mediaPort;
        request->deadline = Clock::now() + std::chrono::milliseconds(std::min(requestedWait, config.requestTimeoutMs));
        std::string rejected;
        {
            std::lock_guard lock(mutex);
            const auto recent = recentTokens.find(peer.id + token);
            if (Stopped()) rejected = "Client is stopping";
            else if (pending || session || !outbound.empty() || state != State::Idle) rejected = "Client is busy";
            else if (recent != recentTokens.end() && recent->second > Clock::now()) rejected = "Session token was already used";
            else if (recentTokens.size() >= MaxTokens) rejected = "Too many recent session requests";
            else {
                request->id = nextRequest++; pending = request; state = State::Busy;
                recentTokens[peer.id + token] = Clock::now() + std::chrono::seconds(60);
                Emit({Event::Kind::IncomingRequest, request->id, peer, token, mediaPort, {}});
            }
        }
        if (!rejected.empty()) { Send(socket->value, Type::Response, Response(token, false, rejected), Clock::now() + std::chrono::milliseconds(500), stopped); return; }
        std::shared_ptr<Session> accepted;
        bool expired{}, peerDisconnected{};
        {
            std::unique_lock lock(mutex);
            while (!Stopped() && request->decision == -1 && Clock::now() < request->deadline) {
                changed.wait_until(lock, std::min(request->deadline, Clock::now() + std::chrono::milliseconds(50)));
                if (request->decision != -1 || Stopped()) break;
                lock.unlock();
                char byte{}; const int count = recv(socket->value, &byte, 1, MSG_PEEK);
                peerDisconnected = count == 0 || count > 0 || (count == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
                lock.lock();
                if (peerDisconnected) break;
            }
            expired = request->decision == -1 || Stopped() || Clock::now() >= request->deadline || peerDisconnected;
            if (pending == request) pending.reset();
            if (!expired && request->decision == 1) {
                accepted = std::make_shared<Session>(); accepted->peer = peer; accepted->token = token;
                accepted->requestId = request->id; accepted->mediaPort = mediaPort; accepted->socket = socket;
                session = accepted; state = State::Receiving;
            } else {
                state = State::Idle;
                if (expired) Emit({Event::Kind::SessionEnded, request->id, peer, token, mediaPort, peerDisconnected ? "Requester disconnected or cancelled" : "Incoming request expired or client stopped"});
            }
        }
        const auto reason = expired ? std::string("Request timed out") : request->reason;
        if (!Send(socket->value, Type::Response, Response(token, accepted != nullptr, reason), Clock::now() + std::chrono::milliseconds(500), stopped)) {
            if (accepted) Finish(accepted, "Requester disconnected before acceptance"); return;
        }
        if (accepted) SessionLoop(accepted);
    }
    void ReceiveHello() {
        std::array<uint8_t, HeaderBytes + MaxPayload + 1> data{}; sockaddr_in from{}; int fromSize = sizeof(from);
        const int count = recvfrom(udp->value, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (count < static_cast<int>(HeaderBytes) || count > static_cast<int>(HeaderBytes + MaxPayload)) return;
        Bytes header(data.begin(), data.begin() + HeaderBytes); Type type{}; uint32_t size{};
        if (!Header(header, type, size) || type != Type::Hello || size + HeaderBytes != static_cast<size_t>(count)) return;
        Bytes payload(data.begin() + HeaderBytes, data.begin() + count); Reader r{payload};
        Peer peer; peer.id = Lower(r.Fixed(36)); peer.controlPort = r.U16(); const auto stateValue = r.U16(); peer.name = r.String(); peer.address = Ip(from);
        if (!r.End() || !Guid(peer.id) || peer.id == config.localId || !peer.controlPort || stateValue > 3 || !Text(peer.name, 96) || peer.address.empty()) return;
        peer.state = static_cast<State>(stateValue);
        std::lock_guard lock(mutex);
        if (peers.size() < MaxPeers || peers.count(peer.id)) peers[peer.id] = {peer, Clock::now()};
    }
    void Main() {
        auto nextHello = Time::min(); auto nextAdapters = Time::min();
        std::vector<DiscoveryTarget> targets;
        while (!Stopped()) {
            const auto now = Clock::now();
            if (now >= nextAdapters) {
                targets = config.testTargets;
                if (config.enableBroadcast) { auto broadcast = BroadcastTargets(config.discoveryPort); targets.insert(targets.end(), broadcast.begin(), broadcast.end()); }
                nextAdapters = now + std::chrono::seconds(10);
            }
            if (now >= nextHello) {
                const auto message = Message(Type::Hello, Hello());
                for (const auto& target : targets) {
                    sockaddr_in address{}; if (!Address(target.address, target.port, address)) continue;
                    sendto(udp->value, reinterpret_cast<const char*>(message.data()), static_cast<int>(message.size()), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
                }
                nextHello = now + std::chrono::milliseconds(config.helloIntervalMs);
                std::lock_guard lock(mutex);
                for (auto it = peers.begin(); it != peers.end();) { if (now - it->second.time >= std::chrono::milliseconds(config.peerTimeoutMs)) it = peers.erase(it); else ++it; }
                for (auto it = recentTokens.begin(); it != recentTokens.end();) { if (it->second <= now) it = recentTokens.erase(it); else ++it; }
            }
            fd_set readable; FD_ZERO(&readable); FD_SET(udp->value, &readable); FD_SET(listener->value, &readable);
            timeval timeout{}; timeout.tv_usec = 25000;
            const auto count = select(0, &readable, nullptr, nullptr, &timeout);
            if (count <= 0) continue;
            if (FD_ISSET(udp->value, &readable)) ReceiveHello();
            if (FD_ISSET(listener->value, &readable)) {
                sockaddr_in from{}; int bytes = sizeof(from);
                auto socket = std::make_shared<Socket>(accept(listener->value, reinterpret_cast<sockaddr*>(&from), &bytes));
                std::string ignored;
                if (socket->value != INVALID_SOCKET && Nonblocking(socket->value, true, ignored)) {
                    const auto address = Ip(from); Spawn([this, socket, address] { HandleRequest(socket, address); });
                }
            }
        }
    }
};

Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() { Stop(); }
bool Service::Start(const Config& config, std::string& error) {
    error.clear(); auto& p = *impl_; std::lock_guard lifecycle(p.lifecycle);
    if (!p.stopping || p.main.joinable()) { error = "Peer service is already running"; return false; }
    sockaddr_in address{};
    if (!Guid(config.localId) || !Text(config.displayName, 96) || !Address(config.bindAddress, config.discoveryPort, address) ||
        config.helloIntervalMs < 25 || config.helloIntervalMs > 10000 || config.peerTimeoutMs < config.helloIntervalMs * 2 || config.peerTimeoutMs > 60000 ||
        config.requestTimeoutMs < 100 || config.requestTimeoutMs > 30000 || config.heartbeatIntervalMs < 25 || config.heartbeatIntervalMs > 10000 ||
        config.sessionTimeoutMs < config.heartbeatIntervalMs * 2 || config.sessionTimeoutMs > 60000 || config.testTargets.size() > 32 ||
        (config.enableBroadcast && config.discoveryPort == 0)) { error = "Invalid peer service configuration"; return false; }
    for (const auto& target : config.testTargets) { sockaddr_in test{}; if (!target.port || !Address(target.address, target.port, test)) { error = "Invalid discovery target"; return false; } }
    if (!NetworkReady(error)) return false;
    auto udp = std::make_shared<Socket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    auto listener = std::make_shared<Socket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (udp->value == INVALID_SOCKET || listener->value == INVALID_SOCKET) return Fail(error, "peer service socket");
    const BOOL yes = TRUE;
    if (setsockopt(udp->value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) ||
        setsockopt(listener->value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) ||
        setsockopt(udp->value, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes))) return Fail(error, "peer socket options");
    if (!Nonblocking(udp->value, false, error) || !Nonblocking(listener->value, true, error)) return false;
    if (bind(udp->value, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) return Fail(error, "UDP discovery bind");
    int addressSize = sizeof(address); if (getsockname(udp->value, reinterpret_cast<sockaddr*>(&address), &addressSize)) return Fail(error, "discovery port");
    const auto udpPort = ntohs(address.sin_port);
    Address(config.bindAddress, config.controlPort, address);
    if (bind(listener->value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) || listen(listener->value, 8)) return Fail(error, "TCP control bind/listen");
    addressSize = sizeof(address); if (getsockname(listener->value, reinterpret_cast<sockaddr*>(&address), &addressSize)) return Fail(error, "control port");
    p.config = config; p.config.localId = Lower(config.localId); p.udp = udp; p.listener = listener;
    p.controlPort = ntohs(address.sin_port); p.discoveryPort = udpPort;
    { std::lock_guard lock(p.mutex); p.state = State::Idle; p.peers.clear(); p.events.clear(); p.recentTokens.clear(); p.pending.reset(); p.session.reset(); p.outbound.clear(); p.outboundPeer.clear(); p.outboundCancel.reset(); }
    p.stopping = false;
    try { p.main = std::thread([&p] { p.Main(); }); }
    catch (...) { p.stopping = true; p.udp.reset(); p.listener.reset(); p.controlPort = 0; p.discoveryPort = 0; error = "Cannot start discovery thread"; return false; }
    return true;
}
void Service::Stop() {
    auto& p = *impl_; std::lock_guard lifecycle(p.lifecycle);
    p.stopping = true; p.changed.notify_all();
    if (p.main.joinable()) p.main.join();
    { std::unique_lock lock(p.mutex); p.changed.wait(lock, [&] { return p.externalRequests == 0; }); }
    std::vector<Impl::Worker> workers;
    { std::lock_guard lock(p.workersMutex); workers.swap(p.workers); }
    for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
    p.udp.reset(); p.listener.reset(); p.controlPort = 0; p.discoveryPort = 0;
    { std::lock_guard lock(p.mutex); p.state = State::Idle; p.pending.reset(); p.session.reset(); p.outbound.clear(); p.outboundPeer.clear(); p.outboundCancel.reset(); }
}
std::vector<Peer> Service::Snapshot() const {
    auto& p = *impl_; std::lock_guard lock(p.mutex); std::vector<Peer> result;
    for (const auto& [_, seen] : p.peers) if (Clock::now() - seen.time < std::chrono::milliseconds(p.config.peerTimeoutMs)) result.push_back(seen.peer);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    return result;
}
void Service::SetState(State state) { auto& p = *impl_; std::lock_guard lock(p.mutex); if (static_cast<unsigned>(state) <= 3) p.state = state; }
uint16_t Service::localControlPort() const { return impl_->controlPort; }
uint16_t Service::localDiscoveryPort() const { return impl_->discoveryPort; }
bool Service::RequestReceive(const Peer& requestedPeer, const std::string& requestedToken, uint16_t mediaPort, uint32_t timeoutMs, std::string& error) {
    error.clear(); auto& p = *impl_;
    Peer peer = requestedPeer; peer.id = Lower(peer.id); const auto token = Lower(requestedToken);
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    if (!Guid(peer.id) || peer.id == p.config.localId || !Hex(token, 32) || !mediaPort || timeoutMs < 100 || timeoutMs > 30000) { error = "Invalid peer/session request"; return false; }
    {
        std::lock_guard lock(p.mutex);
        if (p.Stopped() || p.pending || p.session || !p.outbound.empty()) { error = "Local client is stopped or busy"; return false; }
        const auto recent = p.recentTokens.find(peer.id + token);
        if (recent != p.recentTokens.end() && recent->second > Clock::now()) { error = "Session token was already used"; return false; }
        if (p.recentTokens.size() >= MaxTokens) { error = "Too many recent session requests"; return false; }
        p.outbound = token; p.outboundPeer = peer.id; p.outboundCancel = cancellation; p.state = State::Busy; ++p.externalRequests;
        p.recentTokens[peer.id + token] = Clock::now() + std::chrono::seconds(60);
    }
    struct Guard { Impl& p; std::string token; ~Guard() { std::lock_guard lock(p.mutex); if (p.outbound == token) { p.outbound.clear(); p.outboundPeer.clear(); p.outboundCancel.reset(); p.state = State::Idle; } --p.externalRequests; p.changed.notify_all(); } } guard{p, token};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs); const auto stopped = [&] { return p.Stopped() || cancellation->load(); };
    auto socket = Connect(peer, deadline, stopped, error); if (!socket) return false;
    Bytes request; Fixed(request, p.config.localId); Fixed(request, peer.id); Fixed(request, token); U16(request, mediaPort); U16(request, p.controlPort);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining < 100) { error = "Request deadline expired while connecting"; return false; }
    U32(request, static_cast<uint32_t>(remaining)); String(request, p.config.displayName);
    if (!Send(socket->value, Type::Request, request, deadline, stopped)) { error = "Cannot send receiver request"; return false; }
    Type type{}; Bytes response;
    if (Receive(socket->value, type, response, deadline, stopped) != Io::Ok || type != Type::Response) { error = "Receiver request timed out, was cancelled, or received an invalid reply"; return false; }
    Reader r{response}; const auto responder = Lower(r.Fixed(36)), responseToken = Lower(r.Fixed(32)); const auto accepted = r.U16(); const auto reason = r.String();
    if (!r.End() || responder != peer.id || responseToken != token || accepted > 1 || !Text(reason, 192, true)) { error = "Receiver response identity or format is invalid"; return false; }
    if (!accepted) { error = reason.empty() ? "Receiver rejected the request" : reason; return false; }
    auto session = std::make_shared<Impl::Session>(); session->peer = peer; session->token = token; session->mediaPort = mediaPort; session->socket = socket;
    { std::lock_guard lock(p.mutex); if (stopped()) { error = "Request stopped or cancelled before session activation"; return false; } p.outbound.clear(); p.outboundPeer.clear(); p.outboundCancel.reset(); p.session = session; p.state = State::Sending; }
    if (!p.Spawn([&p, session] { p.SessionLoop(session); })) { p.Finish(session, "Cannot start session monitor"); error = "Cannot start session monitor"; return false; }
    return true;
}
bool Service::Reply(uint64_t requestId, bool accepted, const std::string& reason, std::string& error) {
    error.clear(); auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!Text(reason, 192, true)) { error = "Reply reason must be UTF-8 without control characters, at most 192 bytes"; return false; }
    if (p.Stopped() || !p.pending || p.pending->id != requestId || p.pending->decision != -1 || Clock::now() >= p.pending->deadline) { error = "Incoming request expired or no longer exists"; return false; }
    p.pending->decision = accepted ? 1 : 0; p.pending->reason = reason; p.changed.notify_all(); return true;
}
void Service::EndSession(const Peer& peer, const std::string& token) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (p.session && p.session->peer.id == Lower(peer.id) && p.session->token == Lower(token)) p.session->end = true;
    if (p.outboundCancel && p.outboundPeer == Lower(peer.id) && p.outbound == Lower(token)) p.outboundCancel->store(true);
    if (p.pending && p.pending->peer.id == Lower(peer.id) && p.pending->token == Lower(token)) {
        p.pending->decision = 0; p.pending->reason = "Receiving was cancelled locally"; p.changed.notify_all();
    }
}
std::vector<Event> Service::PollEvents() { auto& p = *impl_; std::lock_guard lock(p.mutex); std::vector<Event> result; result.swap(p.events); return result; }
}
