#include "net/Transport.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>

namespace lan3d::net {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t WireMagic = 0x4c334453; // L3DS
constexpr uint32_t WireVersion = 6;
constexpr uint32_t ModeMagic = 0x4c33444d; // L3DM
constexpr uint32_t CapsMagic = 0x4c334443; // L3DC
constexpr size_t HeaderSize = 80;
constexpr uint32_t HelloMagic = 0x4c334448; // L3DH
constexpr uint32_t AcceptedMagic = 0x4c334441; // L3DA
struct Winsock {
    int status;
    Winsock() { WSADATA data{}; status = WSAStartup(MAKEWORD(2, 2), &data); }
    ~Winsock() { if (!status) WSACleanup(); }
};
bool Ready(std::string& error) {
    static Winsock winsock;
    if (!winsock.status) return true;
    error = "WSAStartup failed: " + std::to_string(winsock.status);
    return false;
}
IoResult SocketError(std::string& error, const char* operation, int code = WSAGetLastError()) {
    error = std::string(operation) + " failed: Winsock " + std::to_string(code);
    return IoResult::Error;
}
bool SetSocket(SOCKET socket, std::string& error) {
    u_long mode = 1;
    const BOOL yes = TRUE;
    const int bufferBytes = 1024 * 1024;
    if (ioctlsocket(socket, FIONBIO, &mode) == SOCKET_ERROR ||
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes)) == SOCKET_ERROR) {
        SocketError(error, "configure TCP socket");
        return false;
    }
    return true;
}
bool Address(const std::string& ip, uint16_t port, sockaddr_in& result, std::string& error) {
    result = {};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &result.sin_addr) != 1) {
        error = "Expected a numeric IPv4 address";
        return false;
    }
    return true;
}
struct SocketState {
    std::atomic<SOCKET> socket{ INVALID_SOCKET };
    std::atomic<bool> stopped{ false };
    // Prevent Close from recycling a SOCKET handle between a readiness check
    // and a nonblocking send/recv/accept call on another thread.
    std::mutex ioMutex;
    ~SocketState() { Close(); }
    void Close() {
        stopped.store(true);
        std::lock_guard<std::mutex> lock(ioMutex);
        const SOCKET old = socket.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) { shutdown(old, SD_BOTH); closesocket(old); }
    }
    void Assign(SOCKET next) { Close(); socket.store(next); stopped.store(false); }
    IoResult Wait(bool write, Clock::time_point deadline, std::string& error) {
        for (;;) {
            const SOCKET s = socket.load();
            if (stopped.load() || s == INVALID_SOCKET) return IoResult::Closed;
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return IoResult::Timeout;
            const auto slice = std::min<int64_t>(remaining, 100000);
            timeval timeout{ static_cast<long>(slice / 1000000), static_cast<long>(slice % 1000000) };
            fd_set ready, failures;
            FD_ZERO(&ready); FD_ZERO(&failures); FD_SET(s, &ready); FD_SET(s, &failures);
            const int value = select(0, write ? nullptr : &ready, write ? &ready : nullptr, &failures, &timeout);
            if (stopped.load()) return IoResult::Closed;
            if (value == SOCKET_ERROR) return SocketError(error, "select");
            if (!value) continue;
            if (FD_ISSET(s, &failures)) {
                int code{}; int length = sizeof(code);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length);
                return SocketError(error, "TCP connection", code);
            }
            if (FD_ISSET(s, &ready)) return IoResult::Ok;
        }
    }
    IoResult Transfer(bool write, uint8_t* bytes, size_t length, Clock::time_point deadline, size_t& total, std::string& error) {
        size_t offset = 0;
        while (offset < length) {
            IoResult result = Wait(write, deadline, error);
            if (result != IoResult::Ok) return result;
            int count{}, code{};
            {
                std::lock_guard<std::mutex> lock(ioMutex);
                const SOCKET s = socket.load();
                if (stopped.load() || s == INVALID_SOCKET) return IoResult::Closed;
                const int size = static_cast<int>(std::min<size_t>(length - offset, 1024 * 1024));
                count = write ? send(s, reinterpret_cast<const char*>(bytes + offset), size, 0) : recv(s, reinterpret_cast<char*>(bytes + offset), size, 0);
                if (count == SOCKET_ERROR) code = WSAGetLastError();
            }
            if (count == 0) return IoResult::Closed;
            if (count == SOCKET_ERROR) {
                if (stopped.load()) return IoResult::Closed;
                if (code == WSAEWOULDBLOCK) continue;
                return SocketError(error, write ? "send" : "recv", code);
            }
            offset += count; total += count;
        }
        return IoResult::Ok;
    }
};
Clock::time_point Deadline(uint32_t milliseconds) {
    return Clock::now() + std::chrono::milliseconds(std::clamp(milliseconds, 1u, 300000u));
}
void Put32(uint8_t* dst, uint32_t value) {
    dst[0] = uint8_t(value >> 24); dst[1] = uint8_t(value >> 16); dst[2] = uint8_t(value >> 8); dst[3] = uint8_t(value);
}
uint32_t Get32(const uint8_t* src) {
    return (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) | (uint32_t(src[2]) << 8) | src[3];
}
void Put64(uint8_t* dst, uint64_t value) { Put32(dst, uint32_t(value >> 32)); Put32(dst + 4, uint32_t(value)); }
uint64_t Get64(const uint8_t* src) { return (uint64_t(Get32(src)) << 32) | Get32(src + 4); }
bool ValidAudioHeader(uint64_t timestamp, uint32_t frames, uint32_t flags, uint64_t videoTimestamp) {
    if ((flags & ~1u) || frames > audio::MaxBlockFrames) return false;
    if (!frames) return !timestamp && !flags;
    const uint64_t duration = uint64_t(frames) * 10000000 / audio::SampleRate;
    return timestamp && duration <= videoTimestamp && timestamp <= videoTimestamp - duration;
}
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool TokenBytes(const std::string& token, std::array<uint8_t, 20>& hello) {
    if (token.size() != 32) return false;
    Put32(hello.data(), HelloMagic);
    for (size_t i = 0; i < 16; ++i) {
        const int hi = HexDigit(token[i * 2]), lo = HexDigit(token[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        hello[i + 4] = uint8_t((hi << 4) | lo);
    }
    return true;
}
}

bool ValidSessionToken(const std::string& token) {
    std::array<uint8_t, 20> hello{};
    return TokenBytes(token, hello);
}
struct TcpConnection::Impl : SocketState {
    bool sentAny{}, receivedAny{}, authenticated{}, capabilities{};
    std::array<uint8_t, 8> modeBytes{};
    size_t modeRead{};
    Clock::time_point modeStarted{};
};
TcpConnection::TcpConnection() : impl_(std::make_unique<Impl>()) {}
TcpConnection::~TcpConnection() = default;
TcpConnection::TcpConnection(TcpConnection&&) noexcept = default;
TcpConnection& TcpConnection::operator=(TcpConnection&&) noexcept = default;
void TcpConnection::Close() { if (impl_) impl_->Close(); }
bool TcpConnection::IsOpen() const { return impl_ && impl_->socket.load() != INVALID_SOCKET && !impl_->stopped.load(); }
bool TcpConnection::PollPeerClosed() {
    if (!impl_) return true;
    {
        std::unique_lock<std::mutex> lock(impl_->ioMutex, std::try_to_lock);
        if (!lock.owns_lock()) return !IsOpen();
        const SOCKET socket = impl_->socket.load();
        if (impl_->stopped.load() || socket == INVALID_SOCKET) return true;
        char byte{};
        const int result = recv(socket, &byte, 1, MSG_PEEK);
        if (result > 0 || (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)) return false;
    }
    // Close acquires ioMutex itself. Never call it while holding the peek lock.
    impl_->Close();
    return true;
}
IoResult TcpConnection::Connect(const std::string& ipv4, uint16_t port, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->Close(); impl_->sentAny = false; impl_->receivedAny = false; impl_->authenticated = false; impl_->capabilities = false; impl_->modeRead = 0;
    if (!Ready(error)) return IoResult::Error;
    sockaddr_in address{};
    if (!port || !Address(ipv4, port, address, error)) { if (error.empty()) error = "TCP port must not be zero"; return IoResult::Error; }
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return SocketError(error, "socket");
    impl_->Assign(s);
    if (!SetSocket(s, error)) { impl_->Close(); return IoResult::Error; }
    const int connected = connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { SocketError(error, "connect"); impl_->Close(); return IoResult::Error; }
    if (connected == SOCKET_ERROR) {
        const IoResult result = impl_->Wait(true, Deadline(timeoutMs), error);
        if (result != IoResult::Ok) { impl_->Close(); return result; }
        int code{}; int length = sizeof(code);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) == SOCKET_ERROR || code) {
            SocketError(error, "connect", code ? code : WSAGetLastError()); impl_->Close(); return IoResult::Error;
        }
    }
    return IoResult::Ok;
}
IoResult TcpConnection::AuthenticateClient(const std::string& token, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    std::array<uint8_t, 20> hello{};
    if (!TokenBytes(token, hello) || impl_->sentAny || impl_->receivedAny || impl_->authenticated || impl_->capabilities) {
        error = "Invalid session token or authentication attempted after stream start";
        impl_->Close(); return IoResult::Error;
    }
    const auto deadline = Deadline(timeoutMs);
    size_t total{};
    auto result = impl_->Transfer(true, hello.data(), hello.size(), deadline, total, error);
    std::array<uint8_t, 4> accepted{};
    if (result == IoResult::Ok) result = impl_->Transfer(false, accepted.data(), accepted.size(), deadline, total, error);
    if (result == IoResult::Ok && Get32(accepted.data()) != AcceptedMagic) {
        error = "Sender did not accept the desktop session"; result = IoResult::Error;
    }
    if (result == IoResult::Ok) impl_->authenticated = true;
    else {
        if (error.empty()) error = result == IoResult::Timeout ? "Session authentication timed out" : "Session authentication was rejected or disconnected";
        impl_->Close();
    }
    return result;
}
IoResult TcpConnection::AuthenticateServer(const std::string& token, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    std::array<uint8_t, 20> expected{}, hello{};
    if (!TokenBytes(token, expected) || impl_->sentAny || impl_->receivedAny || impl_->authenticated || impl_->capabilities) {
        error = "Invalid session token or authentication attempted after stream start";
        impl_->Close(); return IoResult::Error;
    }
    const auto deadline = Deadline(timeoutMs);
    size_t total{};
    auto result = impl_->Transfer(false, hello.data(), hello.size(), deadline, total, error);
    if (result == IoResult::Ok) {
        unsigned mismatch{};
        for (size_t i = 0; i < hello.size(); ++i) mismatch |= unsigned(hello[i] ^ expected[i]);
        if (mismatch) { error = "Desktop session authentication rejected"; result = IoResult::Error; }
    }
    std::array<uint8_t, 4> accepted{};
    Put32(accepted.data(), AcceptedMagic);
    if (result == IoResult::Ok) result = impl_->Transfer(true, accepted.data(), accepted.size(), deadline, total, error);
    if (result == IoResult::Ok) impl_->authenticated = true;
    else {
        if (error.empty()) error = result == IoResult::Timeout ? "Session authentication timed out" : "Session authentication disconnected";
        impl_->Close();
    }
    return result;
}
IoResult TcpConnection::SendCapabilities(uint32_t hardwareDecoders, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    if ((hardwareDecoders & ~7u) || impl_->sentAny || impl_->receivedAny || impl_->capabilities) {
        error = "Invalid or repeated decoder capability preface"; impl_->Close(); return IoResult::Error;
    }
    std::array<uint8_t, 12> header{};
    Put32(header.data(), CapsMagic); Put32(header.data() + 4, WireVersion); Put32(header.data() + 8, hardwareDecoders);
    size_t total{};
    const auto result = impl_->Transfer(true, header.data(), header.size(), Deadline(timeoutMs), total, error);
    if (result != IoResult::Ok) impl_->Close(); else impl_->capabilities = true;
    return result;
}
IoResult TcpConnection::ReceiveCapabilities(uint32_t& hardwareDecoders, uint32_t timeoutMs, std::string& error) {
    error.clear(); hardwareDecoders = 0;
    if (!IsOpen()) return IoResult::Closed;
    if (impl_->sentAny || impl_->receivedAny || impl_->capabilities) {
        error = "Decoder capability negotiation attempted after stream start"; impl_->Close(); return IoResult::Error;
    }
    std::array<uint8_t, 12> header{};
    size_t total{};
    auto result = impl_->Transfer(false, header.data(), header.size(), Deadline(timeoutMs), total, error);
    if (result == IoResult::Ok && (Get32(header.data()) != CapsMagic || Get32(header.data() + 4) != WireVersion || (Get32(header.data() + 8) & ~7u))) {
        error = "Incompatible media protocol; update both clients to the same version"; result = IoResult::Error;
    }
    if (result != IoResult::Ok) impl_->Close();
    else { hardwareDecoders = Get32(header.data() + 8); impl_->capabilities = true; }
    return result;
}
IoResult TcpConnection::SendStereoMode(bool enabled, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    if (!impl_->capabilities) { error = "Mode control requires capability negotiation"; return IoResult::Error; }
    std::array<uint8_t, 8> message{};
    Put32(message.data(), ModeMagic); Put32(message.data() + 4, enabled ? 1u : 0u);
    size_t total{};
    const auto result = impl_->Transfer(true, message.data(), message.size(), Deadline(timeoutMs), total, error);
    if (result != IoResult::Ok) impl_->Close();
    return result;
}
IoResult TcpConnection::PollStereoMode(bool& enabled, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    if (!impl_->capabilities) { error = "Mode control requires capability negotiation"; return IoResult::Error; }
    bool received{};
    for (unsigned i = 0; i < 32; ++i) {
        if (impl_->modeRead && Clock::now() - impl_->modeStarted > std::chrono::seconds(2)) {
            error = "Incomplete receiver mode message timed out"; impl_->Close(); return IoResult::Error;
        }
        int count{}, code{};
        {
            std::lock_guard lock(impl_->ioMutex);
            const auto socket = impl_->socket.load();
            if (socket == INVALID_SOCKET || impl_->stopped.load()) return IoResult::Closed;
            count = recv(socket, reinterpret_cast<char*>(impl_->modeBytes.data() + impl_->modeRead),
                int(impl_->modeBytes.size() - impl_->modeRead), 0);
            if (count == SOCKET_ERROR) code = WSAGetLastError();
        }
        if (count == SOCKET_ERROR && code == WSAEWOULDBLOCK) return received ? IoResult::Ok : IoResult::Timeout;
        if (count <= 0) {
            impl_->Close();
            if (count == SOCKET_ERROR) return SocketError(error, "receiver mode", code);
            if (impl_->modeRead) { error = "Truncated receiver mode message"; return IoResult::Error; }
            return IoResult::Closed;
        }
        if (!impl_->modeRead) impl_->modeStarted = Clock::now();
        impl_->modeRead += count;
        if (impl_->modeRead != impl_->modeBytes.size()) continue;
        if (Get32(impl_->modeBytes.data()) != ModeMagic || Get32(impl_->modeBytes.data() + 4) > 1) {
            error = "Invalid receiver mode message"; impl_->Close(); return IoResult::Error;
        }
        enabled = Get32(impl_->modeBytes.data() + 4) != 0;
        impl_->modeRead = 0; received = true;
    }
    return received ? IoResult::Ok : IoResult::Timeout;
}
IoResult TcpConnection::Send(const media::EncodedFrame& packet, uint32_t timeoutMs, std::string& error) {
    error.clear();
    if (!IsOpen()) return IoResult::Closed;
    if (!media::ValidPacket(packet) || (!impl_->sentAny && !packet.keyframe)) { error = "Invalid video packet or connection did not start with a keyframe"; return IoResult::Error; }
    if (!audio::ValidBlock(packet.audio) || !ValidAudioHeader(packet.audio.timestamp100ns, packet.audio.Frames(),
        packet.audio.discontinuity ? 1u : 0u, packet.timestamp100ns)) {
        error = "Invalid audio block, size, or audio interval beyond its video timestamp";
        return IoResult::Error;
    }
    std::array<uint8_t, HeaderSize> header{};
    Put32(header.data(), WireMagic); Put32(header.data() + 4, WireVersion); Put32(header.data() + 8, HeaderSize);
    Put32(header.data() + 12, (packet.keyframe ? 1u : 0u) | (packet.kind == ContentKind::LeiaSbs ? 2u : packet.kind == ContentKind::LeiaLeft ? 4u : 0u));
    Put32(header.data() + 16, packet.config.width); Put32(header.data() + 20, packet.config.height);
    Put32(header.data() + 24, packet.config.fps); Put32(header.data() + 28, packet.config.bitrate);
    Put64(header.data() + 32, packet.sequence); Put64(header.data() + 40, packet.timestamp100ns);
    Put32(header.data() + 48, static_cast<uint32_t>(packet.config.sequenceHeader.size()));
    Put32(header.data() + 52, static_cast<uint32_t>(packet.data.size())); Put32(header.data() + 56, static_cast<uint32_t>(packet.config.codec));
    Put32(header.data() + 60, (packet.displayWidth << 16) | packet.displayHeight);
    Put64(header.data() + 64, packet.audio.timestamp100ns);
    Put32(header.data() + 72, packet.audio.Frames()); Put32(header.data() + 76, packet.audio.discontinuity ? 1u : 0u);
    std::vector<uint8_t> pcm(packet.audio.samples.size() * sizeof(int16_t));
    for (size_t i = 0; i < packet.audio.samples.size(); ++i) {
        const uint16_t sample = static_cast<uint16_t>(packet.audio.samples[i]);
        pcm[i * 2] = uint8_t(sample); pcm[i * 2 + 1] = uint8_t(sample >> 8);
    }
    const auto deadline = Deadline(timeoutMs);
    size_t total{};
    IoResult result = impl_->Transfer(true, header.data(), header.size(), deadline, total, error);
    if (result == IoResult::Ok) result = impl_->Transfer(true, const_cast<uint8_t*>(packet.config.sequenceHeader.data()), packet.config.sequenceHeader.size(), deadline, total, error);
    if (result == IoResult::Ok) result = impl_->Transfer(true, const_cast<uint8_t*>(packet.data.data()), packet.data.size(), deadline, total, error);
    if (result == IoResult::Ok) result = impl_->Transfer(true, pcm.data(), pcm.size(), deadline, total, error);
    if (result != IoResult::Ok) {
        impl_->Close();
        if (result == IoResult::Timeout && total) { error = "TCP send deadline exceeded; partial packet discarded by closing connection"; return IoResult::Error; }
    } else impl_->sentAny = true;
    return result;
}
IoResult TcpConnection::Receive(media::EncodedFrame& packet, uint32_t timeoutMs, std::string& error) {
    error.clear(); packet = {};
    if (!IsOpen()) return IoResult::Closed;
    std::array<uint8_t, HeaderSize> header{};
    const auto deadline = Deadline(timeoutMs);
    size_t total{};
    // Read the fixed prefix first so old clients receive a version diagnostic
    // immediately, without waiting for 16 bytes that their v3 header never sends.
    IoResult result = impl_->Transfer(false, header.data(), 12, deadline, total, error);
    if (result == IoResult::Ok && (Get32(header.data()) != WireMagic || Get32(header.data() + 4) != WireVersion || Get32(header.data() + 8) != HeaderSize)) {
        error = "Incompatible media protocol; update both clients to the same version";
        result = IoResult::Error;
    }
    if (result == IoResult::Ok) result = impl_->Transfer(false, header.data() + 12, header.size() - 12, deadline, total, error);
    if (result == IoResult::Ok) {
        const auto* h = header.data();
        packet.keyframe = (Get32(h + 12) & 1) != 0;
        packet.kind = (Get32(h + 12) & 4) ? ContentKind::LeiaLeft : (Get32(h + 12) & 2) ? ContentKind::LeiaSbs : ContentKind::Desktop;
        packet.config.width = Get32(h + 16); packet.config.height = Get32(h + 20);
        packet.config.fps = Get32(h + 24); packet.config.bitrate = Get32(h + 28);
        packet.config.codec = static_cast<media::CodecId>(Get32(h + 56));
        packet.displayWidth = Get32(h + 60) >> 16; packet.displayHeight = Get32(h + 60) & 0xffffu;
        packet.sequence = Get64(h + 32); packet.timestamp100ns = Get64(h + 40);
        const uint32_t extra = Get32(h + 48), size = Get32(h + 52);
        const uint32_t audioFrames = Get32(h + 72), audioFlags = Get32(h + 76);
        packet.audio.timestamp100ns = Get64(h + 64); packet.audio.discontinuity = (audioFlags & 1) != 0;
        if (Get32(h) != WireMagic || Get32(h + 4) != WireVersion || Get32(h + 8) != HeaderSize ||
            (Get32(h + 12) & ~7u) || ((Get32(h + 12) & 6u) == 6u) || !ValidDisplaySize(packet.displayWidth, packet.displayHeight) ||
            !media::ValidConfig(packet.config) || extra > media::MaxSequenceHeaderBytes || !size || size > media::MaxEncodedBytes ||
            !ValidAudioHeader(packet.audio.timestamp100ns, audioFrames, audioFlags, packet.timestamp100ns) ||
            (!impl_->receivedAny && !packet.keyframe)) {
            error = "Rejected invalid LAN3D media header, dimensions, size, audio interval, or initial non-keyframe";
            result = IoResult::Error;
        } else {
            packet.config.sequenceHeader.resize(extra); packet.data.resize(size);
            result = impl_->Transfer(false, packet.config.sequenceHeader.data(), extra, deadline, total, error);
            if (result == IoResult::Ok) result = impl_->Transfer(false, packet.data.data(), size, deadline, total, error);
            std::vector<uint8_t> pcm(size_t(audioFrames) * audio::Channels * sizeof(int16_t));
            if (result == IoResult::Ok) result = impl_->Transfer(false, pcm.data(), pcm.size(), deadline, total, error);
            if (result == IoResult::Ok) {
                packet.audio.samples.resize(size_t(audioFrames) * audio::Channels);
                for (size_t i = 0; i < packet.audio.samples.size(); ++i) {
                    const uint16_t sample = uint16_t(pcm[i * 2]) | (uint16_t(pcm[i * 2 + 1]) << 8);
                    packet.audio.samples[i] = std::bit_cast<int16_t>(sample);
                }
            }
        }
    }
    if (result != IoResult::Ok) {
        packet = {};
        if (result != IoResult::Timeout || total) impl_->Close();
        if (result == IoResult::Timeout && total) { error = "TCP receive deadline exceeded; incomplete packet connection closed"; return IoResult::Error; }
        if (result == IoResult::Closed && total) { error = "TCP connection closed during an incomplete media packet"; return IoResult::Error; }
    } else impl_->receivedAny = true;
    return result;
}

struct TcpServer::Impl : SocketState { uint16_t port{}; };
TcpServer::TcpServer() : impl_(std::make_unique<Impl>()) {}
TcpServer::~TcpServer() = default;
void TcpServer::Close() { impl_->Close(); }
uint16_t TcpServer::port() const { return impl_->port; }
bool TcpServer::Listen(const std::string& ipv4, uint16_t port, std::string& error) {
    error.clear(); impl_->Close(); impl_->port = 0;
    if (!Ready(error)) return false;
    sockaddr_in address{};
    if (!Address(ipv4, port, address, error)) return false;
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { SocketError(error, "listen socket"); return false; }
    impl_->Assign(s);
    const BOOL yes = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) == SOCKET_ERROR ||
        !SetSocket(s, error) || bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(s, 1) == SOCKET_ERROR) {
        if (error.empty()) SocketError(error, "listen/bind"); impl_->Close(); return false;
    }
    int length = sizeof(address);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) { SocketError(error, "getsockname"); impl_->Close(); return false; }
    impl_->port = ntohs(address.sin_port);
    return true;
}
IoResult TcpServer::Accept(TcpConnection& connection, uint32_t timeoutMs, std::string& error) {
    error.clear();
    const auto deadline = Deadline(timeoutMs);
    for (;;) {
        const IoResult result = impl_->Wait(false, deadline, error);
        if (result != IoResult::Ok) return result;
        SOCKET accepted; int acceptError{};
        {
            std::lock_guard<std::mutex> lock(impl_->ioMutex);
            if (impl_->stopped.load()) return IoResult::Closed;
            accepted = accept(impl_->socket.load(), nullptr, nullptr);
            if (accepted == INVALID_SOCKET) acceptError = WSAGetLastError();
        }
        if (accepted == INVALID_SOCKET) {
            if (impl_->stopped.load()) return IoResult::Closed;
            if (acceptError == WSAEWOULDBLOCK) continue;
            return SocketError(error, "accept", acceptError);
        }
        if (!SetSocket(accepted, error)) { closesocket(accepted); return IoResult::Error; }
        connection.Close();
        if (!connection.impl_) connection.impl_ = std::make_unique<TcpConnection::Impl>();
        connection.impl_->Assign(accepted); connection.impl_->sentAny = false; connection.impl_->receivedAny = false; connection.impl_->authenticated = false; connection.impl_->capabilities = false; connection.impl_->modeRead = 0;
        return IoResult::Ok;
    }
}
}
