// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>
namespace gamenet::net { class EventLoop; }
namespace gamenet::websocket {
enum class WebSocketTransportState { NotReady, Ready, Closed };
// WS-C2: owner-local admission/lifecycle, no socket, executor or retry ownership.
class WebSocketTransport {
public:
    virtual ~WebSocketTransport() = default;
    virtual net::EventLoop* ownerLoop() const noexcept = 0;
    virtual WebSocketTransportState state() const noexcept = 0;
    // Accepted owns a copy; refusal owns nothing. No inline receive dispatch.
    // A terminal callback may synchronously re-enter before this returns.
    virtual bool trySend(std::string_view wire) = 0;
    virtual bool shutdown() = 0; // no inline receive dispatch; bounded admission
    virtual void abort() = 0;
    // Seal without force-closing TCP reentrantly from a context destructor.
    virtual void abandon() = 0;
    virtual bool cleanClose() const noexcept = 0;
};
} // namespace gamenet::websocket
