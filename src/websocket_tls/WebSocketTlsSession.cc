// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket_tls/WebSocketTlsSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include <stdexcept>
#include <utility>
namespace gamenet::websocket {
namespace {
class TlsTransport final : public WebSocketTransport {
public:
    explicit TlsTransport(net::EventLoop* loop):loop_(loop) {}
    void bind(const std::shared_ptr<tls::TlsStream>& stream) {stream_=stream;}
    void seal() noexcept {down_=true;}
    net::EventLoop* ownerLoop() const noexcept override {return loop_;}
    WebSocketTransportState state() const noexcept override {
        if(down_ || shutdownQueued_)return WebSocketTransportState::Closed;
        if(const auto s=stream_.lock()) {
            const auto phase=s->snapshot().phase;
            if(phase==tls::TlsPhase::Created || phase==tls::TlsPhase::Handshaking)return WebSocketTransportState::NotReady;
            if(phase==tls::TlsPhase::Open)return WebSocketTransportState::Ready;
        }
        return WebSocketTransportState::Closed;
    }
    bool trySend(std::string_view bytes) override {const auto s=stream_.lock();return s && s->trySend(bytes)==tls::TlsOperation::Accepted;}
    bool shutdown() override {
        if(shutdownQueued_)return true;
        if(stream_.expired())return false;
        // TlsStream::shutdown may drive SSL reads. Leave the active WS parser
        // before doing so; never drop a recursive borrowed plaintext callback.
        const auto weak=stream_;
        if(loop_->executor().post([weak]{if(const auto s=weak.lock())(void)s->shutdown();})!=net::PostResult::Accepted)return false;
        shutdownQueued_=true;return true;
    }
    void abort() override {if(const auto s=stream_.lock())(void)s->abort();}
    void abandon() override {} // wrapper releases TLS; its destructor seals TCP without force-close
    bool cleanClose() const noexcept override {const auto s=stream_.lock();return s && s->snapshot().terminal==tls::TlsError::Graceful;}
private:
    net::EventLoop* loop_;
    std::weak_ptr<tls::TlsStream> stream_;
    bool down_{},shutdownQueued_{};
};
}
struct WebSocketTlsSession::Bridge : std::enable_shared_from_this<Bridge> {
    net::EventLoop* loop;
    WebSocketTlsCallbacks callbacks;
    std::shared_ptr<WebSocketSession> ws;
    std::shared_ptr<TlsTransport> transport;
    WebSocketTlsPhase phase{WebSocketTlsPhase::Created};
    WebSocketTlsResult terminal;
    std::uint64_t exceptions{},posts{},rejections{};
    bool published{},tlsEnded{},wsEnded{},tcpDown{},queued{};
    explicit Bridge(net::EventLoop* owner,WebSocketTlsCallbacks cb):loop(owner),callbacks(std::move(cb)) {
        if(!callbacks.closed)throw std::invalid_argument("WSS requires closed callback");
    }
    void notify() {
        if(!published || phase==WebSocketTlsPhase::Closed || !tlsEnded || !wsEnded)return;
        phase=WebSocketTlsPhase::Closed;
        auto callback=std::move(callbacks.closed);
        callbacks.handshake={};callbacks.ready={};callbacks.message={};callbacks.pong={};
        if(callback)try {callback(terminal);}catch(...){++exceptions;}
    }
    void tlsReady() {
        if(phase!=WebSocketTlsPhase::Handshaking)return;
        phase=WebSocketTlsPhase::Active;
        (void)ws->start(); // WS owns Upgrade admission/deadline; failure aborts TLS
    }
    void data(std::string_view bytes) {
        if(tlsEnded || wsEnded)return;
        net::Buffer input;input.append(bytes);(void)ws->onMessage(input);
    }
    void tlsClosed(tls::TlsError reason) {
        if(!published || tlsEnded)return;
        tlsEnded=true;terminal.tls=reason;phase=WebSocketTlsPhase::Settling;
        ws->onDisconnect(); // bounded WS continuation may own the remaining plaintext
        settle();
    }
    void settle() {
        if(!tlsEnded || phase==WebSocketTlsPhase::Closed || queued)return;
        if(wsEnded){notify();return;}
        try {
            const auto self=shared_from_this();
            if(loop->executor().post([self]{self->queued=false;self->settle();})==net::PostResult::Accepted) {
                queued=true;++posts;return;
            }
        }catch(...){}
        ++rejections;terminal.tls=tls::TlsError::SchedulingRejected;
        (void)ws->abort();notify();
    }
    void wsClosed(WebSocketError reason) {
        if(!published || wsEnded)return;
        wsEnded=true;terminal.websocket=reason;notify();
    }
};
WebSocketTlsSession::WebSocketTlsSession(std::shared_ptr<Bridge> bridge,std::shared_ptr<tls::TlsStream> stream)
    :bridge_(std::move(bridge)),stream_(std::move(stream)) {}
std::shared_ptr<WebSocketTlsSession> WebSocketTlsSession::create(const net::TcpConnectionPtr& connection,
    std::shared_ptr<tls::TlsContext> context,WebSocketTlsCallbacks callbacks,WebSocketTlsOptions options) {
    if(!connection || !context || context->role()!=tls::TlsContext::Role::Server)
        throw std::invalid_argument("WSS requires TCP and server TLS context");
    auto* loop=connection->getLoop();loop->assertInLoopThread();
    auto bridge=std::make_shared<Bridge>(loop,std::move(callbacks));
    auto transport=std::make_shared<TlsTransport>(loop);
    bridge->transport=transport;
    // Validate WS bounds before creating a TLS stream with TCP destructor effects.
    bridge->ws=WebSocketSession::createWithTransport(transport,{
        .handshake=[weak=std::weak_ptr(bridge)](const auto& request) {const auto b=weak.lock();return b && (!b->callbacks.handshake || b->callbacks.handshake(request));},
        .ready=[weak=std::weak_ptr(bridge)] {if(const auto b=weak.lock())if(b->callbacks.ready)b->callbacks.ready();},
        .message=[weak=std::weak_ptr(bridge)](auto type,auto bytes) {if(const auto b=weak.lock())if(b->callbacks.message)b->callbacks.message(type,bytes);},
        .pong=[weak=std::weak_ptr(bridge)](auto bytes) {if(const auto b=weak.lock())if(b->callbacks.pong)b->callbacks.pong(bytes);},
        .closed=[weak=std::weak_ptr(bridge)](auto error) {if(const auto b=weak.lock())b->wsClosed(error);}
    },options.websocket);
    auto stream=tls::TlsStream::create(connection,std::move(context),{
        .ready=[bridge]{bridge->tlsReady();},
        .data=[bridge](auto bytes){bridge->data(bytes);},
        .closed=[bridge](auto error){bridge->tlsClosed(error);}
    },{},options.tls);
    transport->bind(stream);
    auto result=std::shared_ptr<WebSocketTlsSession>(new WebSocketTlsSession(bridge,std::move(stream)));
    bridge->published=true;return result;
}
WebSocketTlsSession::~WebSocketTlsSession() {
    bridge_->loop->assertInLoopThread();
    if(bridge_->phase!=WebSocketTlsPhase::Closed && !bridge_->tcpDown) {
        (void)bridge_->ws->abandon();
    }
    // TLS destructor or its accepted EOF/dispatch lease publishes after cleanup.
}
tls::TlsOperation WebSocketTlsSession::start() {
    if(!bridge_->loop->isInLoopThread())return tls::TlsOperation::WrongThread;
    const auto self=shared_from_this();
    if(bridge_->phase!=WebSocketTlsPhase::Created)return bridge_->phase==WebSocketTlsPhase::Closed?tls::TlsOperation::Closed:tls::TlsOperation::NotReady;
    bridge_->phase=WebSocketTlsPhase::Handshaking;return stream_->start();
}
WebSocketOperation WebSocketTlsSession::send(WebSocketOpcode type,std::string_view bytes) {
    if(!bridge_->loop->isInLoopThread())return WebSocketOperation::WrongThread;
    const auto self=shared_from_this();return bridge_->ws->send(type,bytes);
}
WebSocketOperation WebSocketTlsSession::close(std::uint16_t code,std::string_view reason) {
    if(!bridge_->loop->isInLoopThread())return WebSocketOperation::WrongThread;
    const auto self=shared_from_this();return bridge_->ws->close(code,reason);
}
WebSocketOperation WebSocketTlsSession::abort() {
    if(!bridge_->loop->isInLoopThread())return WebSocketOperation::WrongThread;
    const auto self=shared_from_this();return bridge_->ws->abort();
}
tls::TlsOperation WebSocketTlsSession::onMessage(net::Buffer& bytes) {
    if(!bridge_->loop->isInLoopThread())return tls::TlsOperation::WrongThread;
    const auto self=shared_from_this();return stream_->onMessage(bytes);
}
void WebSocketTlsSession::onWritable() {
    bridge_->loop->assertInLoopThread();const auto self=shared_from_this();stream_->onWritable();bridge_->ws->onWritable();
}
void WebSocketTlsSession::onDisconnect() {
    bridge_->loop->assertInLoopThread();const auto self=shared_from_this();bridge_->tcpDown=true;bridge_->transport->seal();stream_->onDisconnect();
}
WebSocketTlsSnapshot WebSocketTlsSession::snapshot() const {
    bridge_->loop->assertInLoopThread();return {bridge_->phase,bridge_->terminal,bridge_->ws->snapshot(),stream_->snapshot(),bridge_->exceptions,bridge_->posts,bridge_->rejections};
}
net::EventLoopExecutor WebSocketTlsSession::ownerExecutor() const noexcept {return bridge_->loop->executor();}
} // namespace gamenet::websocket
