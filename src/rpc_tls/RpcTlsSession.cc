// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc_tls/RpcTlsSession.h"
#include "gamenet/rpc/RpcTransport.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include <stdexcept>
#include <utility>

namespace gamenet::rpc {
namespace {
class TlsTransport final : public RpcTransport {
public:
    explicit TlsTransport(net::EventLoop* loop) : loop_(loop) {}
    void bind(const std::shared_ptr<tls::TlsStream>& stream) { stream_ = stream; }
    net::EventLoop* ownerLoop() const noexcept override { return loop_; }
    RpcTransportState state() const noexcept override {
        const auto stream = stream_.lock();
        if (!stream) return RpcTransportState::Closed;
        switch (stream->snapshot().phase) {
        case tls::TlsPhase::Created:
        case tls::TlsPhase::Handshaking: return RpcTransportState::NotReady;
        case tls::TlsPhase::Open: return RpcTransportState::Ready;
        default: return RpcTransportState::Closed;
        }
    }
    bool trySend(std::string_view wire) override {
        const auto stream = stream_.lock();
        return stream && stream->trySend(wire) == tls::TlsOperation::Accepted;
    }
    void abort() override { if (auto stream = stream_.lock()) (void)stream->abort(); }
private:
    net::EventLoop* loop_;
    std::weak_ptr<tls::TlsStream> stream_;
};
}  // namespace

// TLS owns its callbacks/bridge; bridge owns RPC, which only weakly observes TLS.
// This acyclic lease survives raw TCP context release until verified input settles.
struct RpcTlsSession::Bridge : std::enable_shared_from_this<Bridge> {
    net::EventLoop* loop;
    net::EventLoopExecutor executor;
    RpcTlsCallbacks callbacks;
    std::shared_ptr<RpcChannel> rpc;
    RpcTlsPhase phase{RpcTlsPhase::Created};
    tls::TlsError terminal{tls::TlsError::None};
    std::uint64_t posts{}, rejections{}, callbackExceptions{};
    bool queued{}, tcpDown{}, published{};

    Bridge(net::EventLoop* owner, RpcTlsCallbacks cb)
        : loop(owner), executor(owner->executor()), callbacks(std::move(cb)) {
        if (!callbacks.closed) throw std::invalid_argument("RPC/TLS requires a closed callback");
    }
    bool owner() const noexcept { return loop->isInLoopThread(); }
    bool ending() const noexcept { return phase == RpcTlsPhase::Settling || phase == RpcTlsPhase::Closed; }
    void ready() {
        if (phase != RpcTlsPhase::Handshaking) return;
        phase = RpcTlsPhase::Ready;
        if (callbacks.ready) callbacks.ready(); // TLS contains exceptions and reports CallbackException
    }
    void data(std::string_view bytes) {
        if (ending() || rpc->snapshot().closed) return;
        net::Buffer input; input.append(bytes);
        rpc->onMessage(input); // RPC copies/consumes before dispatch; no borrowed TLS span survives
    }
    void notifyClosed() {
        if (phase == RpcTlsPhase::Closed) return;
        phase = RpcTlsPhase::Closed;
        auto closed = std::move(callbacks.closed);
        callbacks.ready = {};
        if (closed) try { closed(terminal); } catch (...) { ++callbackExceptions; }
    }
    void settle() {
        if (phase != RpcTlsPhase::Settling || queued) return;
        const auto state = rpc->snapshot();
        if (state.dispatching || state.continuationQueued) {
            // One bounded owner task waits for existing RPC work. It never
            // processes buffered handlers recursively or owns a borrowed span.
            try {
                const auto self = shared_from_this();
                const auto result = executor.post([self] { self->queued = false; self->settle(); });
                if (result == net::PostResult::Accepted) { queued = true; ++posts; return; }
            } catch (...) {}
            ++rejections;
            terminal = tls::TlsError::SchedulingRejected;
        }
        rpc->onDisconnect();
        notifyClosed();
    }
    void end(tls::TlsError reason) {
        if (!published || ending()) return;
        phase = RpcTlsPhase::Settling;
        terminal = reason;
        settle();
    }
    void abandon() {
        if (!published || ending() || tcpDown) return;
        phase = RpcTlsPhase::Settling;
        terminal = tls::TlsError::Aborted;
        (void)rpc->close();
        notifyClosed();
    }
};

RpcTlsSession::RpcTlsSession(std::shared_ptr<Bridge> bridge, std::shared_ptr<tls::TlsStream> stream)
    : bridge_(std::move(bridge)), stream_(std::move(stream)) {}
std::shared_ptr<RpcTlsSession> RpcTlsSession::create(const net::TcpConnectionPtr& connection,
    std::shared_ptr<tls::TlsContext> context, RpcTlsCallbacks callbacks,
    RpcChannel::Handler handler, RpcTlsOptions options) {
    if (!connection || !context) throw std::invalid_argument("RPC/TLS requires connection and context");
    auto* loop = connection->getLoop(); loop->assertInLoopThread();
    auto bridge = std::make_shared<Bridge>(loop, std::move(callbacks));
    // Validate RPC bounds before constructing TLS: abandoning an unpublished
    // stream would otherwise seal a caller's still-usable TCP connection.
    auto transport = std::make_shared<TlsTransport>(loop);
    bridge->rpc = RpcChannel::createWithTransport(transport, std::move(handler), options.rpc);
    auto stream = tls::TlsStream::create(connection, std::move(context), {
        .ready = [bridge] { bridge->ready(); },
        .data = [bridge](auto bytes) { bridge->data(bytes); },
        .closed = [bridge](auto error) { bridge->end(error); }
    }, std::move(options.peerName), options.tls);
    transport->bind(stream);
    auto result = std::shared_ptr<RpcTlsSession>(new RpcTlsSession(bridge, std::move(stream)));
    bridge->published = true;
    return result;
}
RpcTlsSession::~RpcTlsSession() {
    bridge_->loop->assertInLoopThread();
    bridge_->abandon(); // TLS destructor uses its existing non-reentrant TCP shutdown policy
}
std::shared_ptr<RpcChannel> RpcTlsSession::channel() const {
    bridge_->loop->assertInLoopThread(); return bridge_->rpc;
}
tls::TlsOperation RpcTlsSession::start() {
    if (!bridge_->owner()) return tls::TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (bridge_->phase != RpcTlsPhase::Created) return bridge_->ending() ?
        tls::TlsOperation::Closed : tls::TlsOperation::NotReady;
    bridge_->phase = RpcTlsPhase::Handshaking;
    return stream_->start();
}
tls::TlsOperation RpcTlsSession::shutdown() {
    if (!bridge_->owner()) return tls::TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (bridge_->ending()) return tls::TlsOperation::Closed;
    if (bridge_->phase == RpcTlsPhase::Closing) return tls::TlsOperation::Accepted;
    bridge_->phase = RpcTlsPhase::Closing;
    (void)bridge_->rpc->close();
    if (!bridge_->ending()) (void)stream_->shutdown();
    return tls::TlsOperation::Accepted;
}
tls::TlsOperation RpcTlsSession::abort() {
    if (!bridge_->owner()) return tls::TlsOperation::WrongThread;
    const auto self = shared_from_this();
    if (bridge_->ending()) return tls::TlsOperation::Closed;
    bridge_->phase = RpcTlsPhase::Closing;
    (void)bridge_->rpc->close();
    if (!bridge_->ending()) (void)stream_->abort();
    return tls::TlsOperation::Accepted;
}
tls::TlsOperation RpcTlsSession::onMessage(net::Buffer& ciphertext) {
    if (!bridge_->owner()) return tls::TlsOperation::WrongThread;
    const auto self = shared_from_this(); return stream_->onMessage(ciphertext);
}
void RpcTlsSession::onWritable() {
    bridge_->loop->assertInLoopThread(); const auto self = shared_from_this(); stream_->onWritable();
}
void RpcTlsSession::onDisconnect() {
    bridge_->loop->assertInLoopThread(); const auto self = shared_from_this();
    bridge_->tcpDown = true;
    stream_->onDisconnect();
}
RpcTlsSnapshot RpcTlsSession::snapshot() const {
    bridge_->loop->assertInLoopThread();
    return {bridge_->phase, bridge_->terminal, bridge_->rpc->snapshot(), stream_->snapshot(),
        bridge_->posts, bridge_->rejections, bridge_->callbackExceptions};
}
net::EventLoopExecutor RpcTlsSession::ownerExecutor() const noexcept { return bridge_->executor; }
}  // namespace gamenet::rpc
