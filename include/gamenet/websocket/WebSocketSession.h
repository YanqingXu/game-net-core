// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket/WebSocketCodec.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include <chrono>
#include <functional>
#include <memory>

namespace gamenet::net { class Buffer; }
namespace gamenet::websocket {
enum class WebSocketPhase { Created, Handshaking, Upgrading, Open, Closing, Rejecting, Closed };
enum class WebSocketError { None, Graceful, HandshakeRejected, HandshakeTimeout,
    ProtocolError, InvalidUtf8, MessageTooLarge, Backpressure, TransportError,
    CloseTimeout, CallbackException, SchedulingRejected, Aborted, Truncated };
enum class WebSocketOperation { Accepted, WrongThread, Closed, NotReady, Overloaded, InvalidArgument, OwnerUnavailable };
struct WebSocketOptions {
    WebSocketCodecOptions codec;
    std::size_t maxHeaderBytes{8192}, maxHeaderFields{32}, maxControlFrames{4}, maxControlBytes{1024};
    std::chrono::milliseconds handshakeTimeout{5000}, closeTimeout{2000}, retryDelay{5};
};
struct WebSocketSnapshot {
    WebSocketPhase phase{WebSocketPhase::Created};
    WebSocketError terminal{WebSocketError::None};
    std::size_t inputBytes{}, fragmentedBytes{}, pendingControlBytes{}, pendingControls{};
    std::uint64_t enqueuedControlBytes{}, admittedControlBytes{}, discardedControlBytes{};
    std::uint64_t receivedMessages{}, acceptedSends{}, rejectedSends{}, controlRetries{}, callbackExceptions{}, reentrantMessages{};
    bool sentClose{}, receivedClose{}, transportDown{}, continuationQueued{}, closeTimedOut{};
    unsigned httpStatus{};
};
struct WebSocketCallbacks {
    std::function<bool(const WebSocketRequest&)> handshake; // optional application target/Origin policy
    std::function<void()> ready;
    std::function<void(WebSocketOpcode, std::string_view)> message; // borrowed until return
    std::function<void(std::string_view)> pong;
    std::function<void(WebSocketError)> closed;
};
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    static std::shared_ptr<WebSocketSession> create(const net::TcpConnectionPtr& connection,
        WebSocketCallbacks callbacks = {}, WebSocketOptions options = {});
    ~WebSocketSession(); // owner-only; terminal cleanup without reentrant TCP force-close
    WebSocketSession(const WebSocketSession&) = delete;
    WebSocketSession& operator=(const WebSocketSession&) = delete;
    WebSocketOperation start();
    WebSocketOperation send(WebSocketOpcode type, std::string_view payload); // Text/Binary/Ping only
    WebSocketOperation close(std::uint16_t code = 1000, std::string_view reason = {});
    WebSocketOperation abort();
    WebSocketOperation onMessage(net::Buffer& input);
    void onWritable();
    void onDisconnect();
    WebSocketSnapshot snapshot() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;
private:
    struct Impl;
    explicit WebSocketSession(std::unique_ptr<Impl> impl);
    void drive();
    std::unique_ptr<Impl> impl_;
};
} // namespace gamenet::websocket
