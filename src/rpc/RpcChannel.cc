// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/rpc/RpcTransport.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gamenet::rpc {
namespace {
using Clock = std::chrono::steady_clock;
// The compatibility factory still weakly observes TCP and uses Core admission.
class TcpTransport final : public RpcTransport {
public:
    explicit TcpTransport(const net::TcpConnectionPtr& connection)
        : connection_(connection), loop_(connection->getLoop()) {}
    net::EventLoop* ownerLoop() const noexcept override { return loop_; }
    RpcTransportState state() const noexcept override {
        const auto connection = connection_.lock();
        return connection && connection->connected() ? RpcTransportState::Ready : RpcTransportState::Closed;
    }
    bool trySend(std::string_view wire) override {
        const auto connection = connection_.lock();
        return connection && connection->trySend(wire) == net::TcpSendResult::Accepted;
    }
    void abort() override { if (auto connection = connection_.lock()) connection->forceClose(); }
private:
    std::weak_ptr<net::TcpConnection> connection_;
    net::EventLoop* loop_;
};
constexpr auto overhead = RpcCodec::kHeaderBytes + protocol::PacketFramer::kLengthBytes;
protocol::PacketFramerOptions framing(const RpcOptions& options) {
    if (options.maxPayloadBytes > (std::numeric_limits<std::uint32_t>::max)() - overhead ||
        options.maxPendingRequests == 0 || options.maxPendingBytes == 0 ||
        options.maxInboundRequests == 0 || options.maxInboundBytes == 0 ||
        options.maxFramesPerDispatch == 0 ||
        options.maxBufferedBytes < options.maxPayloadBytes + overhead ||
        options.maxRequestTimeout.count() <= 0 ||
        options.maxRequestTimeout.count() > (std::numeric_limits<std::uint32_t>::max)())
        throw std::invalid_argument("invalid RPC bounds");
    protocol::PacketFramerOptions result;
    result.maxPayloadBytes = options.maxPayloadBytes + RpcCodec::kHeaderBytes;
    result.maxBufferedBytes = options.maxBufferedBytes;
    result.maxFramesPerPush = options.maxFramesPerDispatch;
    result.maxFrameBytesPerPush = options.maxPayloadBytes + overhead;
    return result;
}
}  // namespace

struct RpcChannel::Impl {
    enum class Phase { Open, Closed };
    struct Pending {
        Completion completion;
        net::TimerId timer;
        Clock::time_point deadline;
        std::size_t bytes;
        bool admitted{false};
    };
    struct Inbound { net::TimerId timer; Clock::time_point deadline; std::size_t bytes; };
    std::shared_ptr<RpcTransport> transport;
    net::EventLoop* loop;
    net::EventLoopExecutor executor;
    RpcOptions options;
    Handler handler;
    protocol::PacketFramer framer;
    std::weak_ptr<RpcChannel> channel;
    std::unordered_map<std::uint64_t, Pending> pending;
    std::unordered_map<std::uint64_t, Inbound> inbound;
    RpcSnapshot stats;
    Phase phase{Phase::Open};
    std::uint64_t lastIssued{}, lastReceived{};
    bool dispatching{false}, continuationQueued{false};

    Impl(std::shared_ptr<RpcTransport> pipe, Handler cb, RpcOptions opts)
        : transport(std::move(pipe)), loop(transport->ownerLoop()), executor(loop->executor()),
          options(opts), handler(std::move(cb)), framer(framing(opts)) {}
    bool owner() const noexcept { return loop->isInLoopThread(); }
    bool open() const noexcept { return phase == Phase::Open; }
    void invoke(Completion cb, RpcResult result) noexcept {
        try { cb(std::move(result)); } catch (...) { ++stats.callbackExceptions; }
    }
    void finish(std::uint64_t id, RpcResult result) {
        auto node = pending.extract(id);
        if (node.empty()) return;
        auto& entry = node.mapped();
        stats.pendingBytes -= entry.bytes;
        (void)loop->tryCancel(entry.timer);
        if (entry.admitted) {
            ++stats.completedCalls;
            invoke(std::move(entry.completion), std::move(result));
        }
    }
    void closeAll(RpcStatus reason) {
        if (!open()) return;
        phase = Phase::Closed;
        auto outgoing = std::move(pending);
        pending.clear();
        for (const auto& [id, entry] : inbound) (void)loop->tryCancel(entry.timer);
        inbound.clear();
        stats.pendingBytes = stats.inboundBytes = 0;
        framer.reset();
        for (const auto& [id, entry] : outgoing) {
            (void)loop->tryCancel(entry.timer);
            if (entry.admitted) ++stats.completedCalls;
        }
        for (auto& [id, entry] : outgoing)
            if (entry.admitted) invoke(std::move(entry.completion), {reason, {}});
    }
    void fail(RpcStatus reason) {
        closeAll(reason);
        try { transport->abort(); } catch (...) { ++stats.transportExceptions; }
    }
    bool send(RpcMessage message) {
        if (!open() || transport->state() != RpcTransportState::Ready) return false;
        const auto body = RpcCodec::encode(message, options.maxPayloadBytes);
        if (!body) return false;
        const auto wire = framer.encode(*body);
        if (!wire) return false;
        try { return transport->trySend(*wire); }
        catch (...) { ++stats.transportExceptions; return false; }
    }
    void error(std::uint64_t id, std::string_view text) {
        // Even a zero-payload configuration can send a bounded terminal error.
        try {
            if (send({RpcMessageKind::Error, id, 0, 0,
                    std::string(text.substr(0, options.maxPayloadBytes))})) return;
        } catch (...) {}
        fail(RpcStatus::Disconnected);
    }
    void receive(RpcMessage message) {
        if (message.kind != RpcMessageKind::Request) {
            const auto it = pending.find(message.requestId);
            if (it == pending.end() || !it->second.admitted) {
                ++stats.ignoredResponses;
                return;
            }
            const auto status = Clock::now() >= it->second.deadline ? RpcStatus::Timeout :
                message.kind == RpcMessageKind::Response ? RpcStatus::Ok : RpcStatus::RemoteError;
            finish(message.requestId, {status, status == RpcStatus::Timeout ?
                std::string{} : std::move(message.payload)});
            return;
        }
        if (message.requestId <= lastReceived) { fail(RpcStatus::ProtocolError); return; }
        lastReceived = message.requestId;
        // A TLS terminal can precede buffered RPC continuation retirement.
        // Responses may still settle existing calls; sealed transports must
        // not start new business handlers or attempt an error write.
        if (transport->state() != RpcTransportState::Ready) { ++stats.inboundRejected; return; }
        const auto weight = message.payload.size() + overhead;
        if (inbound.size() >= options.maxInboundRequests ||
            weight > options.maxInboundBytes - stats.inboundBytes) {
            ++stats.inboundRejected;
            error(message.requestId, "overloaded");
            return;
        }
        const auto timeout = std::min(std::chrono::milliseconds(message.timeoutMs), options.maxRequestTimeout);
        const auto id = message.requestId;
        const auto deadline = Clock::now() + timeout;
        const auto weak = channel;
        const auto timer = loop->tryRunAfter(timeout, [weak, id] {
            if (auto self = weak.lock()) self->expire(id, true);
        });
        if (timer.result != net::PostResult::Accepted) {
            ++stats.inboundRejected;
            error(id, "owner unavailable");
            return;
        }
        try { inbound.emplace(id, Inbound{timer.timerId, deadline, weight}); }
        catch (...) { (void)loop->tryCancel(timer.timerId); throw; }
        stats.inboundBytes += weight;
        if (!handler) { (void)replyError(id, "unknown method"); return; }
        try {
            handler(message, [weak, id, owner = executor](RpcResult result) {
                // A foreign caller must not acquire a temporary strong lease:
                // racing owner teardown could then destroy the channel there.
                if (!owner.isInOwnerThread())
                    return weak.expired() || !owner.available()
                        ? RpcOperation::Closed : RpcOperation::WrongThread;
                const auto self = weak.lock();
                return self ? self->reply(id, std::move(result)) : RpcOperation::Closed;
            });
        } catch (...) {
            ++stats.handlerExceptions;
            if (inbound.contains(id)) (void)replyError(id, "handler exception");
        }
    }
    RpcOperation replyError(std::uint64_t id, std::string_view message) {
        if (auto self = channel.lock()) return self->reply(id, {RpcStatus::RemoteError,
            std::string(message.substr(0, options.maxPayloadBytes))});
        return RpcOperation::Closed;
    }
    void dispatch(protocol::FrameResult result) {
        dispatching = true;
        try {
            if (result.status == protocol::FrameStatus::FrameTooLarge ||
                result.status == protocol::FrameStatus::BufferLimitExceeded || framer.faulted())
                fail(RpcStatus::ProtocolError);
            for (const auto& frame : result.frames) {
                if (!open()) break;
                auto message = RpcCodec::decode(frame, options.maxPayloadBytes);
                if (!message) { fail(RpcStatus::ProtocolError); break; }
                receive(std::move(*message));
            }
            if (open() && result.needsContinuation && !continuationQueued) {
                const auto self = channel.lock();
                const auto posted = executor.post([self] {
                    self->impl_->continuationQueued = false;
                    self->drain();
                });
                if (posted == net::PostResult::Accepted) continuationQueued = true;
                else {
                    ++stats.continuationRejections;
                    fail(RpcStatus::SchedulingRejected);
                }
            }
        } catch (...) { fail(RpcStatus::ProtocolError); }
        dispatching = false;
    }
};

RpcChannel::RpcChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::shared_ptr<RpcChannel> RpcChannel::create(
    const net::TcpConnectionPtr& connection, Handler handler, RpcOptions options) {
    if (!connection) throw std::invalid_argument("RPC requires a connection");
    connection->getLoop()->assertInLoopThread();
    return createWithTransport(std::make_shared<TcpTransport>(connection), std::move(handler), options);
}
std::shared_ptr<RpcChannel> RpcChannel::createWithTransport(
    std::shared_ptr<RpcTransport> transport, Handler handler, RpcOptions options) {
    if (!transport || !transport->ownerLoop()) throw std::invalid_argument("RPC requires an owner transport");
    transport->ownerLoop()->assertInLoopThread();
    auto result = std::shared_ptr<RpcChannel>(new RpcChannel(
        std::make_unique<Impl>(std::move(transport), std::move(handler), options)));
    result->impl_->channel = result;
    return result;
}
RpcChannel::~RpcChannel() {
    impl_->loop->assertInLoopThread();
    impl_->closeAll(RpcStatus::Shutdown);
}
net::EventLoopExecutor RpcChannel::ownerExecutor() const noexcept { return impl_->executor; }
RpcSnapshot RpcChannel::snapshot() const {
    impl_->loop->assertInLoopThread();
    auto result = impl_->stats;
    result.closed = !impl_->open();
    result.pendingRequests = impl_->pending.size();
    result.inboundRequests = impl_->inbound.size();
    result.dispatching = impl_->dispatching;
    result.continuationQueued = impl_->continuationQueued;
    return result;
}
RpcCallResult RpcChannel::call(std::uint32_t method, std::string_view payload,
    std::chrono::milliseconds timeout, Completion completion) {
    auto& s = *impl_;
    if (!s.owner()) return {RpcOperation::WrongThread, 0};
    const auto self = shared_from_this();
    if (!s.open()) return {RpcOperation::Closed, 0};
    const auto transportState = s.transport->state();
    if (transportState == RpcTransportState::NotReady) return {RpcOperation::NotReady, 0};
    if (transportState != RpcTransportState::Ready) return {RpcOperation::Closed, 0};
    if (method == 0 || !completion || payload.size() > s.options.maxPayloadBytes ||
        timeout.count() <= 0 || timeout > s.options.maxRequestTimeout)
        return {RpcOperation::InvalidArgument, 0};
    const auto weight = payload.size() + overhead;
    if (s.pending.size() >= s.options.maxPendingRequests ||
        weight > s.options.maxPendingBytes - s.stats.pendingBytes)
        return {RpcOperation::PendingLimit, 0};
    if (s.lastIssued == (std::numeric_limits<std::uint64_t>::max)())
        return {RpcOperation::IdExhausted, 0};
    if (s.loop->phase() != net::EventLoopPhase::Running) return {RpcOperation::OwnerUnavailable, 0};
    const auto id = ++s.lastIssued;
    net::TimerId timerId;
    try {
        const auto weak = weak_from_this();
        const auto deadline = Clock::now() + timeout;
        const auto timer = s.loop->tryRunAfter(timeout, [weak, id] {
            if (auto channel = weak.lock()) channel->expire(id, false);
        });
        if (timer.result != net::PostResult::Accepted) return {RpcOperation::OwnerUnavailable, 0};
        timerId = timer.timerId;
        s.pending.emplace(id, Impl::Pending{std::move(completion), timerId, deadline, weight});
        s.stats.pendingBytes += weight;
        const bool sent = s.send({RpcMessageKind::Request, id, method,
            static_cast<std::uint32_t>(timeout.count()), std::string(payload)});
        const auto it = s.pending.find(id);
        if (!sent || !s.open() || s.transport->state() != RpcTransportState::Ready || it == s.pending.end()) {
            s.finish(id, {RpcStatus::Disconnected, {}}); // not admitted: no callback
            return {RpcOperation::SendRejected, 0};
        }
        it->second.admitted = true;
        ++s.stats.acceptedCalls;
        return {RpcOperation::Accepted, id};
    } catch (...) {
        (void)s.loop->tryCancel(timerId);
        s.finish(id, {RpcStatus::Disconnected, {}});
        return {RpcOperation::OwnerUnavailable, 0};
    }
}
RpcOperation RpcChannel::cancel(std::uint64_t id) {
    if (!impl_->owner()) return RpcOperation::WrongThread;
    const auto self = shared_from_this();
    if (!impl_->pending.contains(id)) return RpcOperation::NotFound;
    impl_->finish(id, {RpcStatus::Cancelled, {}});
    return RpcOperation::Accepted;
}
RpcOperation RpcChannel::close() {
    if (!impl_->owner()) return RpcOperation::WrongThread;
    const auto self = shared_from_this();
    impl_->closeAll(RpcStatus::Shutdown);
    return RpcOperation::Accepted;
}
void RpcChannel::onDisconnect() {
    impl_->loop->assertInLoopThread();
    const auto self = shared_from_this();
    impl_->closeAll(RpcStatus::Disconnected);
}
void RpcChannel::onMessage(net::Buffer& input) {
    impl_->loop->assertInLoopThread();
    const auto self = shared_from_this();
    if (impl_->dispatching) { ++impl_->stats.reentrantMessages; return; }
    if (!impl_->open()) { input.retrieveAll(); return; }
    try {
        auto result = impl_->framer.push(std::string_view(input.peek(), input.readableBytes()));
        input.retrieveAll();
        impl_->dispatch(std::move(result));
    } catch (...) { input.retrieveAll(); impl_->fail(RpcStatus::ProtocolError); }
}
void RpcChannel::drain() {
    try { if (impl_->open()) impl_->dispatch(impl_->framer.push({})); }
    catch (...) { impl_->fail(RpcStatus::ProtocolError); }
}
void RpcChannel::expire(std::uint64_t id, bool inbound) {
    if (!inbound) { impl_->finish(id, {RpcStatus::Timeout, {}}); return; }
    auto node = impl_->inbound.extract(id);
    if (node.empty()) return;
    impl_->stats.inboundBytes -= node.mapped().bytes;
    impl_->error(id, "deadline exceeded");
}
RpcOperation RpcChannel::reply(std::uint64_t id, RpcResult result) {
    auto& s = *impl_;
    if (!s.owner()) return RpcOperation::WrongThread;
    const auto self = shared_from_this();
    if (!s.open()) return RpcOperation::Closed;
    if ((result.status != RpcStatus::Ok && result.status != RpcStatus::RemoteError) ||
        result.payload.size() > s.options.maxPayloadBytes) return RpcOperation::InvalidArgument;
    const auto it = s.inbound.find(id);
    if (it == s.inbound.end()) return RpcOperation::NotFound;
    if (Clock::now() >= it->second.deadline) { expire(id, true); return RpcOperation::NotFound; }
    const auto timer = it->second.timer;
    s.stats.inboundBytes -= it->second.bytes;
    s.inbound.erase(it);
    (void)s.loop->tryCancel(timer);
    try {
        if (s.send({result.status == RpcStatus::Ok ? RpcMessageKind::Response : RpcMessageKind::Error,
                id, 0, 0, std::move(result.payload)})) return RpcOperation::Accepted;
    } catch (...) {}
    s.fail(RpcStatus::Disconnected);
    return RpcOperation::SendRejected;
}
}  // namespace gamenet::rpc
