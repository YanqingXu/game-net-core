// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/tls/TlsStream.h"
namespace gamenet::websocket {
enum class WebSocketTlsPhase { Created, Handshaking, Active, Settling, Closed };
struct WebSocketTlsOptions { WebSocketOptions websocket; tls::TlsOptions tls; };
struct WebSocketTlsResult {
    WebSocketError websocket{WebSocketError::None};
    tls::TlsError tls{tls::TlsError::None};
    bool graceful() const noexcept {return websocket==WebSocketError::Graceful && tls==tls::TlsError::Graceful;}
};
struct WebSocketTlsCallbacks {
    std::function<bool(const WebSocketRequest&)> handshake;
    std::function<void()> ready;
    std::function<void(WebSocketOpcode,std::string_view)> message;
    std::function<void(std::string_view)> pong;
    std::function<void(WebSocketTlsResult)> closed; // required; after both cleanups
};
struct WebSocketTlsSnapshot {
    WebSocketTlsPhase phase{WebSocketTlsPhase::Created};
    WebSocketTlsResult terminal;
    WebSocketSnapshot websocket;
    tls::TlsSnapshot tls;
    std::uint64_t callbackExceptions{}, settlementPosts{}, settlementRejections{};
};
// Explicit server composition; raw TCP callbacks carry ciphertext only.
class WebSocketTlsSession : public std::enable_shared_from_this<WebSocketTlsSession> {
public:
    static std::shared_ptr<WebSocketTlsSession> create(const net::TcpConnectionPtr& connection,
        std::shared_ptr<tls::TlsContext> context, WebSocketTlsCallbacks callbacks,
        WebSocketTlsOptions options = {});
    ~WebSocketTlsSession(); // owner-only; non-reentrant abandonment
    WebSocketTlsSession(const WebSocketTlsSession&) = delete;
    WebSocketTlsSession& operator=(const WebSocketTlsSession&) = delete;
    tls::TlsOperation start();
    WebSocketOperation send(WebSocketOpcode type,std::string_view payload);
    WebSocketOperation close(std::uint16_t code=1000,std::string_view reason={});
    WebSocketOperation abort();
    tls::TlsOperation onMessage(net::Buffer& ciphertext);
    void onWritable();
    void onDisconnect(); // forward actual TCP-down before releasing context
    WebSocketTlsSnapshot snapshot() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;
private:
    struct Bridge;
    WebSocketTlsSession(std::shared_ptr<Bridge>,std::shared_ptr<tls::TlsStream>);
    std::shared_ptr<Bridge> bridge_;
    std::shared_ptr<tls::TlsStream> stream_;
};
} // namespace gamenet::websocket
