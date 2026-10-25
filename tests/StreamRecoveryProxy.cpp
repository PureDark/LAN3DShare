// Loopback-only wire-v5 fault injector for the production media reconnect test.
// It forwards real authentication/capabilities unchanged, truncates exactly one
// access unit, and accepts a fresh connection without restarting either client.
#include "../src/common/Frame.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : value_(s) {}
    ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    SOCKET Get() const { return value_; }
    void Close() {
        if (value_ == INVALID_SOCKET) return;
        shutdown(value_, SD_BOTH); closesocket(value_); value_ = INVALID_SOCKET;
    }
private:
    SOCKET value_{INVALID_SOCKET};
};
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Nonblocking(SOCKET s) {
    u_long mode = 1; const BOOL yes = TRUE;
    Check(ioctlsocket(s, FIONBIO, &mode) != SOCKET_ERROR, "Cannot make proxy socket nonblocking");
    Check(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes)) != SOCKET_ERROR, "Cannot disable proxy Nagle");
}
bool Ready(SOCKET s, bool write, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        fd_set set; FD_ZERO(&set); FD_SET(s, &set); timeval slice{0, 100000};
        const int result = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &slice);
        if (result == SOCKET_ERROR) return false;
        if (result > 0) return true;
    }
    return false;
}
bool Transfer(SOCKET s, bool write, uint8_t* bytes, size_t size, Clock::time_point deadline) {
    size_t offset{};
    while (offset < size) {
        if (!Ready(s, write, deadline)) return false;
        const int chunk = int((std::min)(size - offset, size_t(1024 * 1024)));
        const int done = write ? send(s, reinterpret_cast<const char*>(bytes + offset), chunk, 0)
            : recv(s, reinterpret_cast<char*>(bytes + offset), chunk, 0);
        if (done == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (done <= 0) return false;
        offset += size_t(done);
    }
    return true;
}
uint32_t U32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t U64(const uint8_t* p) { return (uint64_t(U32(p)) << 32) | U32(p + 4); }
sockaddr_in Loopback(uint16_t port) {
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); return address;
}
unsigned Number(const wchar_t* value, unsigned low, unsigned high) {
    size_t used{}; const auto n = std::stoul(value, &used);
    Check(used == wcslen(value) && n >= low && n <= high, "Invalid proxy numeric argument"); return unsigned(n);
}
void Handshake(SOCKET client, SOCKET sender, Clock::time_point deadline) {
    std::array<uint8_t, 20> hello{};
    Check(Transfer(client, false, hello.data(), hello.size(), deadline) && U32(hello.data()) == 0x4c334448, "Missing real L3DH token handshake");
    Check(Transfer(sender, true, hello.data(), hello.size(), deadline), "Forwarding token failed");
    std::array<uint8_t, 4> accepted{};
    Check(Transfer(sender, false, accepted.data(), accepted.size(), deadline) && U32(accepted.data()) == 0x4c334441, "Production sender rejected token");
    Check(Transfer(client, true, accepted.data(), accepted.size(), deadline), "Forwarding authentication response failed");
    std::array<uint8_t, 12> caps{};
    Check(Transfer(client, false, caps.data(), caps.size(), deadline) && U32(caps.data()) == 0x4c334443 && U32(caps.data() + 4) == 6 && !(U32(caps.data() + 8) & ~7u), "Missing real v6 decoder capabilities");
    Check(Transfer(sender, true, caps.data(), caps.size(), deadline), "Forwarding decoder capabilities failed");
    std::array<uint8_t, 8> mode{};
    Check(Transfer(client, false, mode.data(), mode.size(), deadline) && U32(mode.data()) == 0x4c33444d && U32(mode.data() + 4) <= 1,
        "Missing initial receiver display mode");
    Check(Transfer(sender, true, mode.data(), mode.size(), deadline), "Forwarding display mode failed");
}
}

int wmain(int argc, wchar_t** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    int result = 1;
    try {
        unsigned upstreamPort{}, stallMs{700}, beforeFault{8}, timeoutMs{20000};
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            Check(i + 1 < argc, "Missing proxy option value");
            const auto value = argv[++i];
            if (option == L"--upstream-port") upstreamPort = Number(value, 1, 65535);
            else if (option == L"--stall-ms") stallMs = Number(value, 300, 3000);
            else if (option == L"--before-fault") beforeFault = Number(value, 4, 100);
            else if (option == L"--timeout-ms") timeoutMs = Number(value, 3000, 30000);
            else throw std::runtime_error("Unknown proxy argument");
        }
        Check(upstreamPort != 0, "--upstream-port is required");
        Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        Check(listener.Get() != INVALID_SOCKET, "Cannot create proxy listener");
        const BOOL exclusive = TRUE;
        Check(setsockopt(listener.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != SOCKET_ERROR, "Cannot make proxy port exclusive");
        const auto local = Loopback(0);
        Check(bind(listener.Get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != SOCKET_ERROR && listen(listener.Get(), 2) != SOCKET_ERROR, "Cannot bind proxy loopback listener");
        sockaddr_in bound{}; int length = sizeof(bound);
        Check(getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&bound), &length) != SOCKET_ERROR, "Cannot query proxy port");
        Nonblocking(listener.Get());
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        std::cout << "PROXY_READY port=" << ntohs(bound.sin_port) << '\n' << std::flush;
        unsigned faultCount{}, resumedPackets{}, skippedAttempts{};
        uint64_t firstSequence{}, resumedSequence{};
        for (unsigned connection = 1; connection <= 2;) {
            Check(Ready(listener.Get(), false, deadline), "Timed out waiting for receiver reconnect");
            Socket client(accept(listener.Get(), nullptr, nullptr));
            Check(client.Get() != INVALID_SOCKET, "Accepting receiver failed"); Nonblocking(client.Get());
            Socket sender(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            const auto target = Loopback(uint16_t(upstreamPort));
            Check(sender.Get() != INVALID_SOCKET && connect(sender.Get(), reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != SOCKET_ERROR, "Connecting production sender failed"); Nonblocking(sender.Get());
            unsigned completed{}, audioPackets{};
            uint64_t audioSampleFrames{};
            try {
            // While the first socket is deliberately stalled, the receiver may
            // queue then abandon more than one reconnect. A stale accepted
            // socket is not a media generation: discard it and keep listening.
            const auto handshakeDeadline = (std::min)(deadline, Clock::now() + std::chrono::milliseconds(1500));
            Handshake(client.Get(), sender.Get(), handshakeDeadline);
            while (Clock::now() < deadline) {
                std::array<uint8_t, 80> header{};
                if (!Transfer(sender.Get(), false, header.data(), header.size(), deadline)) break;
                const uint32_t extra = U32(header.data() + 48), bytes = U32(header.data() + 52), flags = U32(header.data() + 12);
                const uint32_t audioFrames = U32(header.data() + 72), audioFlags = U32(header.data() + 76);
                const uint64_t audioTimestamp = U64(header.data() + 64), videoTimestamp = U64(header.data() + 40);
                const uint32_t display = U32(header.data() + 60);
                Check(U32(header.data()) == 0x4c334453 && U32(header.data() + 4) == 6 && U32(header.data() + 8) == 80 && !(flags & ~7u) && (flags & 6u) != 6u &&
                    lan3d::ValidDisplaySize(display >> 16, display & 0xffffu), "Malformed v5 access-unit header");
                Check(extra <= 64u * 1024 && bytes > 1 && bytes <= 32u * 1024 * 1024, "Unbounded proxy access unit");
                Check(audioFrames <= 4800 && !(audioFlags & ~1u), "Unbounded audio block or invalid flags");
                const uint64_t duration = uint64_t(audioFrames) * 10000000 / 48000;
                Check(audioFrames ? audioTimestamp && duration <= videoTimestamp && audioTimestamp <= videoTimestamp - duration :
                    !audioTimestamp && !audioFlags, "Audio extends beyond its paired video or has invalid empty metadata");
                const auto sequence = U64(header.data() + 32);
                const size_t audioBytes = size_t(audioFrames) * 4;
                std::vector<uint8_t> payload(size_t(extra) + bytes + audioBytes);
                Check(Transfer(sender.Get(), false, payload.data(), payload.size(), deadline), "Incomplete packet from production sender");
                if (completed == 0) {
                    Check((flags & 1) != 0, "Fresh connection did not start at a keyframe");
                    Check(audioFrames && (audioFlags & 1), "Fresh connection must reset its audio timeline");
                }
                if (connection == 1 && completed == beforeFault) {
                    Check(audioBytes >= 4, "Fault packet must contain PCM audio");
                    // Complete video is insufficient: omit the final half of its
                    // paired audio to exercise atomic A/V packet publication.
                    const auto partial = size_t(extra) + bytes + audioBytes / 2;
                    Check(Transfer(client.Get(), true, header.data(), header.size(), deadline) && Transfer(client.Get(), true, payload.data(), partial, deadline), "Could not forward deliberate partial packet");
                    std::cout << "PROXY_FAULT connection=1 completed_packets=" << completed << " sequence=" << sequence << " partial_bytes=" << partial << " payload_bytes=" << payload.size() << " partial_audio=1 audio_frames=" << audioFrames << " stall_ms=" << stallMs << '\n' << std::flush;
                    std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
                    client.Close(); sender.Close(); ++faultCount;
                    std::cout << "PROXY_DISCARDED connection=1 backlog=closed\n" << std::flush;
                    break;
                }
                if (!Transfer(client.Get(), true, header.data(), header.size(), deadline) || !Transfer(client.Get(), true, payload.data(), payload.size(), deadline)) break;
                if (completed == 0) {
                    if (connection == 1) firstSequence = sequence; else resumedSequence = sequence;
                    std::cout << "PROXY_HANDSHAKE connection=" << connection << " token=1 capabilities=1\n"
                        << "PROXY_FIRST_PACKET connection=" << connection << " keyframe=1 sequence=" << sequence << '\n' << std::flush;
                }
                ++completed;
                if (audioFrames) { ++audioPackets; audioSampleFrames += audioFrames; }
            }
            } catch (const std::exception& error) {
                if (completed != 0) throw;
                client.Close(); sender.Close(); ++skippedAttempts;
                std::cout << "PROXY_SKIP_ATTEMPT count=" << skippedAttempts << " generation=" << connection
                    << " reason=" << error.what() << '\n' << std::flush;
                continue;
            }
            if (completed == 0) {
                client.Close(); sender.Close(); ++skippedAttempts;
                std::cout << "PROXY_SKIP_ATTEMPT count=" << skippedAttempts << " generation=" << connection
                    << " reason=closed-before-first-complete-keyframe\n" << std::flush;
                continue;
            }
            if (connection == 1) Check(faultCount == 1 && completed == beforeFault, "Initial connection ended before fault injection");
            else { resumedPackets = completed; Check(completed >= beforeFault, "Reconnected stream did not sustain decoding input"); }
            Check(audioPackets == completed && audioSampleFrames > 0, "A forwarded video packet lost its synthetic PCM audio");
            std::cout << "PROXY_AUDIO connection=" << connection << " packets=" << audioPackets << " sample_frames=" << audioSampleFrames << '\n' << std::flush;
            ++connection;
        }
        Check(resumedSequence > firstSequence, "Reconnect restarted or reused the old source sequence");
        std::cout << "PROXY_FINISHED connections=2 faults=" << faultCount << " resumed_packets=" << resumedPackets << " first_sequence=" << firstSequence << " resumed_sequence=" << resumedSequence << '\n' << std::flush;
        result = 0;
    } catch (const std::exception& error) {
        std::cerr << "StreamRecoveryProxy FAILED: " << error.what() << " (WSA=" << WSAGetLastError() << ")\n";
    }
    WSACleanup(); return result;
}
