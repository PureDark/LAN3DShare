#pragma once
#include "media/Codec.h"
#include <memory>
#include <string>

namespace lan3d::net {
enum class IoResult { Ok, Timeout, Closed, Error };
bool ValidSessionToken(const std::string& token);
// TCP v5 atomically carries one video access unit and its bounded 48 kHz stereo
// S16LE audio interval. Audio ends no later than the paired video's timestamp.
// Close may be called from another thread to interrupt an operation. Join that
// thread before destroying the object or calling Connect/Listen again.
class TcpConnection {
public:
    TcpConnection();
    ~TcpConnection();
    TcpConnection(TcpConnection&&) noexcept;
    TcpConnection& operator=(TcpConnection&&) noexcept;
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    IoResult Connect(const std::string& ipv4, uint16_t port, uint32_t timeoutMs, std::string& error);
    // Optional desktop-session preface, before any video packet. Client sends
    // "L3DH" + 16 token bytes, server verifies and replies "L3DA". Both sides
    // close on any failure/timeout; callers without a token skip only the preface.
    IoResult AuthenticateClient(const std::string& token, uint32_t timeoutMs, std::string& error);
    IoResult AuthenticateServer(const std::string& token, uint32_t timeoutMs, std::string& error);
    // Before video: receiver advertises its actual hardware decoder mask.
    // H.264 software decoding is the common fallback, even when the mask is 0.
    IoResult SendCapabilities(uint32_t hardwareDecoders, uint32_t timeoutMs, std::string& error);
    IoResult ReceiveCapabilities(uint32_t& hardwareDecoders, uint32_t timeoutMs, std::string& error);
    IoResult Send(const media::EncodedFrame& packet, uint32_t timeoutMs, std::string& error);
    // Idle timeout preserves connection. Partial-packet timeout closes it so a
    // subsequent Receive can never mistake a payload fragment for a new header.
    // A peer EOF between packets returns Closed; EOF inside a packet is Error.
    IoResult Receive(media::EncodedFrame& packet, uint32_t timeoutMs, std::string& error);
    void Close();
    bool IsOpen() const;
    // Nonblocking idle-stream check. Peeks without consuming protocol bytes;
    // EOF/socket errors close the connection, so a paused source can reconnect.
    bool PollPeerClosed();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class TcpServer;
};
class TcpServer {
public:
    TcpServer();
    ~TcpServer();
    bool Listen(const std::string& bindIPv4, uint16_t port, std::string& error);
    IoResult Accept(TcpConnection& connection, uint32_t timeoutMs, std::string& error);
    void Close();
    uint16_t port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
