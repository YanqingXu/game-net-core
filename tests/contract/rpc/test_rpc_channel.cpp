// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/rpc/RpcTransport.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/RpcStateSequence.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace gamenet::rpc;
using gamenet::net::Buffer;
namespace {
void transportAdmission() {
    struct Transport final : RpcTransport {
        gamenet::net::EventLoop* loop;
        RpcTransportState phase{RpcTransportState::NotReady};
        bool accepts{true};
        std::function<void()> duringSend;
        explicit Transport(gamenet::net::EventLoop& owner) : loop(&owner) {}
        gamenet::net::EventLoop* ownerLoop() const noexcept override { return loop; }
        RpcTransportState state() const noexcept override { return phase; }
        bool trySend(std::string_view) override { if (duringSend) duringSend(); return accepts; }
        void abort() override { phase = RpcTransportState::Closed; }
    };
    gamenet::net::EventLoop loop;
    auto transport = std::make_shared<Transport>(loop);
    auto channel = RpcChannel::createWithTransport(transport);
    unsigned completed = 0;
    const auto rejected = channel->call(1, "before ready", 1s, [&](auto) { ++completed; });
    GAMENET_TEST_ASSERT(rejected.status == RpcOperation::NotReady && rejected.requestId == 0);
    GAMENET_TEST_ASSERT(channel->snapshot().pendingRequests == 0 && channel->snapshot().pendingBytes == 0);
    transport->phase = RpcTransportState::Ready;
    const auto first = channel->call(1, "ready", 1s, [&](auto r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::Cancelled); ++completed;
    });
    GAMENET_TEST_ASSERT(first.status == RpcOperation::Accepted && first.requestId == 1);
    (void)channel->cancel(first.requestId);
    transport->accepts = false;
    const auto refused = channel->call(1, "full", 1s, [&](auto) { ++completed; });
    GAMENET_TEST_ASSERT(refused.status == RpcOperation::SendRejected && refused.requestId == 0 && completed == 1);
    transport->duringSend = [weak = std::weak_ptr(channel)] { if (auto c = weak.lock()) c->onDisconnect(); };
    const auto down = channel->call(1, "down during admission", 1s, [&](auto) { ++completed; });
    const auto s = channel->snapshot();
    GAMENET_TEST_ASSERT(down.status == RpcOperation::SendRejected && down.requestId == 0 && completed == 1);
    GAMENET_TEST_ASSERT(s.closed && s.pendingRequests == 0 && s.pendingBytes == 0 &&
        s.acceptedCalls == 1 && s.completedCalls == 1);
    channel.reset();
    transport->duringSend = {}; transport->accepts = true; transport->phase = RpcTransportState::Ready;
    unsigned handlers = 0, drained = 0;
    channel = RpcChannel::createWithTransport(transport, [&](const auto&, auto) { ++handlers; });
    const auto pending = channel->call(1, "received before EOF", 1s, [&](auto result) {
        GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok); ++drained;
    });
    GAMENET_TEST_ASSERT(pending.status == RpcOperation::Accepted);
    transport->phase = RpcTransportState::Closed;
    auto wire = [](RpcMessage message) {
        gamenet::protocol::PacketFramer framer;
        return *framer.encode(*RpcCodec::encode(message));
    };
    const auto received = wire({RpcMessageKind::Request, 1, 1, 1000, "sealed"}) +
        wire({RpcMessageKind::Response, pending.requestId, 0, 0, "ok"});
    Buffer input; input.append(std::string_view(received)); channel->onMessage(input);
    GAMENET_TEST_ASSERT(handlers == 0 && drained == 1 && channel->snapshot().inboundRejected == 1 &&
        channel->snapshot().pendingRequests == 0);
    channel->onDisconnect(); channel.reset(); loop.quit(); loop.loop();
}
struct Fixture {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair sockets;
    gamenet::net::TcpConnectionPtr connection;
    std::shared_ptr<RpcChannel> rpc;
    explicit Fixture(RpcOptions options = {}, RpcChannel::Handler handler = {},
        gamenet::net::EventLoopOptions loopOptions = {}, std::size_t sendLimit = 1024 * 1024)
        : loop(loopOptions), connection(gamenet::test::makeTcpConnection(loop, sockets, "rpc")) {
        connection->setBackpressureOptions({.lowWaterMarkBytes = sendLimit / 4,
            .highWaterMarkBytes = sendLimit / 2, .hardLimitBytes = sendLimit});
        rpc = RpcChannel::create(connection, std::move(handler), options);
        connection->setMessageCallback([&](const auto&, Buffer* input) { rpc->onMessage(*input); });
        connection->setConnectionCallback([&](const auto& conn) {
            if (!conn->connected() && rpc) rpc->onDisconnect();
        });
        connection->setCloseCallback([](const auto& conn) { conn->connectDestroyed(); });
        connection->setCallbackExceptionHandler([](const auto&, const auto&) {
            GAMENET_TEST_FAIL("RPC leaked a callback exception into Core");
        });
        connection->connectEstablished();
    }
    ~Fixture() {
        if (rpc) (void)rpc->close();
        connection->forceClose();
        if (loop.phase() == gamenet::net::EventLoopPhase::Running) { loop.quit(); loop.loop(); }
    }
    static std::string wire(RpcMessage message) {
        gamenet::protocol::PacketFramer framer;
        return *framer.encode(*RpcCodec::encode(message));
    }
    void feed(std::string_view bytes) { Buffer input; input.append(bytes); rpc->onMessage(input); }
    void response(std::uint64_t id, std::string payload = "ok") {
        feed(wire({RpcMessageKind::Response, id, 0, 0, std::move(payload)}));
    }
    void run(std::chrono::milliseconds duration) {
        loop.runAfter(duration, [&] { connection->forceClose(); loop.quit(); });
        loop.loop();
    }
};
void limitsAndReentry() {
    RpcOptions options; options.maxPendingRequests = 2; options.maxPendingBytes = 64;
    Fixture f(options);
    unsigned callbacks = 0, rejectedCallbacks = 0;
    const auto a = f.rpc->call(1, "a", 1s, [&](RpcResult result) {
        GAMENET_TEST_ASSERT(result.status == RpcStatus::Cancelled);
        ++callbacks;
        const auto next = f.rpc->call(1, "c", 1s, [&](RpcResult r) {
            GAMENET_TEST_ASSERT(r.status == RpcStatus::Cancelled); ++callbacks;
        });
        GAMENET_TEST_ASSERT(next.status == RpcOperation::Accepted);
        GAMENET_TEST_ASSERT(f.rpc->cancel(next.requestId) == RpcOperation::Accepted);
    });
    const auto b = f.rpc->call(2, "b", 1s, [&](RpcResult result) {
        GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok && result.payload == "ok"); ++callbacks;
    });
    GAMENET_TEST_ASSERT(a.status == RpcOperation::Accepted && b.status == RpcOperation::Accepted);
    const auto rejected = f.rpc->call(1, "", 1s, [&](auto) { ++rejectedCallbacks; });
    GAMENET_TEST_ASSERT(rejected.status == RpcOperation::PendingLimit && rejected.requestId == 0);
    GAMENET_TEST_ASSERT(f.rpc->cancel(a.requestId) == RpcOperation::Accepted);
    GAMENET_TEST_ASSERT(f.rpc->cancel(a.requestId) == RpcOperation::NotFound);
    f.response(b.requestId); f.response(b.requestId);
    const auto s = f.rpc->snapshot();
    GAMENET_TEST_ASSERT(callbacks == 3 && rejectedCallbacks == 0 && s.pendingRequests == 0 &&
        s.pendingBytes == 0 && s.acceptedCalls == 3 && s.completedCalls == 3 && s.ignoredResponses == 1);
}
void byteLimitAndSendRejection() {
    RpcOptions options; options.maxPendingBytes = 30;
    Fixture f(options, {}, {}, 64);
    unsigned done = 0;
    GAMENET_TEST_ASSERT(f.rpc->call(1, "abc", 1s, [](auto) {}).status == RpcOperation::PendingLimit);
    auto a = f.rpc->call(1, "ab", 1s, [&](auto) { ++done; });
    GAMENET_TEST_ASSERT(a.status == RpcOperation::Accepted);
    GAMENET_TEST_ASSERT(f.rpc->call(1, "", 1s, [](auto) {}).status == RpcOperation::PendingLimit);
    (void)f.rpc->cancel(a.requestId);
    a = f.rpc->call(1, "", 1s, [&](auto) { ++done; });
    GAMENET_TEST_ASSERT(a.status == RpcOperation::Accepted);
    (void)f.rpc->cancel(a.requestId);
    GAMENET_TEST_ASSERT(done == 2 && f.rpc->snapshot().pendingBytes == 0);
}
void sendRejection() {
    Fixture f({}, {}, {}, 64);
    unsigned done = 0;
    const auto result = f.rpc->call(1, std::string(100, 'x'), 1s, [&](auto) { ++done; });
    GAMENET_TEST_ASSERT(result.status == RpcOperation::SendRejected && result.requestId == 0);
    const auto s = f.rpc->snapshot();
    GAMENET_TEST_ASSERT(done == 0 && s.pendingRequests == 0 && s.pendingBytes == 0 && s.acceptedCalls == 0);
}
void deadlinesAndLateResponses() {
    Fixture f;
    unsigned callbacks = 0;
    const auto a = f.rpc->call(1, "", 1ms, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::Timeout); ++callbacks;
    });
    // Deadline enforcement does not depend on timely TimerQueue dispatch.
    std::this_thread::sleep_for(3ms);
    f.response(a.requestId);
    const auto b = f.rpc->call(1, "", 5ms, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::Timeout); ++callbacks;
    });
    const auto c = f.rpc->call(1, "", 5ms, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::Cancelled); ++callbacks;
    });
    (void)f.rpc->cancel(c.requestId);
    f.loop.runAfter(15ms, [&] { f.response(a.requestId); f.response(b.requestId); f.response(c.requestId); });
    f.run(30ms);
    const auto s = f.rpc->snapshot();
    GAMENET_TEST_ASSERT(callbacks == 3 && s.completedCalls == 3 && s.pendingBytes == 0 && s.ignoredResponses == 3);
}
void disconnectAndDestructor() {
    Fixture f;
    unsigned callbacks = 0;
    for (unsigned i = 0; i < 3; ++i) {
        GAMENET_TEST_ASSERT(f.rpc->call(1, "", 100ms, [&](RpcResult r) {
            GAMENET_TEST_ASSERT(r.status == RpcStatus::Disconnected);
            GAMENET_TEST_ASSERT(f.rpc->snapshot().pendingRequests == 0);
            GAMENET_TEST_ASSERT(f.rpc->call(1, "", 1s, [](auto) {}).status == RpcOperation::Closed);
            ++callbacks;
            if (callbacks == 1) throw std::runtime_error("contained completion");
        }).status == RpcOperation::Accepted);
    }
    f.sockets.closePeer(); // real EOF races the pending deadline
    f.run(150ms);
    GAMENET_TEST_ASSERT(callbacks == 3 && f.rpc->snapshot().callbackExceptions == 1);
    (void)f.rpc->close(); f.rpc->onDisconnect();
    GAMENET_TEST_ASSERT(callbacks == 3);
}
void shutdownAndWeakReply() {
    Fixture f;
    unsigned callbacks = 0;
    (void)f.rpc->call(1, "", 1s, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::Shutdown); ++callbacks;
    });
    f.rpc.reset(); // caller release is also a terminal path
    GAMENET_TEST_ASSERT(callbacks == 1);
}
void inboundAndPartialFrames() {
    RpcOptions options; options.maxInboundRequests = 1; options.maxInboundBytes = 30;
    RpcChannel::Reply saved;
    unsigned handlers = 0;
    Fixture f(options, [&](const RpcMessage& message, RpcChannel::Reply reply) {
        ++handlers;
        GAMENET_TEST_ASSERT(message.method == 7);
        saved = std::move(reply);
        if (message.payload == "e") throw std::runtime_error("contained handler");
    });
    const auto first = Fixture::wire({RpcMessageKind::Request, 1, 7, 1000, "a"});
    f.feed(std::string_view(first).substr(0, 2));
    GAMENET_TEST_ASSERT(handlers == 0);
    f.feed(std::string_view(first).substr(2));
    f.feed(Fixture::wire({RpcMessageKind::Request, 2, 7, 1000, "b"}));
    GAMENET_TEST_ASSERT(handlers == 1 && f.rpc->snapshot().inboundRejected == 1);
    GAMENET_TEST_ASSERT(saved({RpcStatus::Ok, "reply"}) == RpcOperation::Accepted);
    GAMENET_TEST_ASSERT(saved({RpcStatus::Ok, "duplicate"}) == RpcOperation::NotFound);
    f.feed(Fixture::wire({RpcMessageKind::Request, 3, 7, 1000, "e"}));
    GAMENET_TEST_ASSERT(f.rpc->snapshot().handlerExceptions == 1 && f.rpc->snapshot().inboundBytes == 0);
    f.feed(Fixture::wire({RpcMessageKind::Request, 4, 7, 5, "t"}));
    f.loop.runAfter(15ms, [&] {
        GAMENET_TEST_ASSERT(saved({RpcStatus::Ok, "late"}) == RpcOperation::NotFound);
        GAMENET_TEST_ASSERT(f.rpc->snapshot().inboundRequests == 0);
    });
    f.run(30ms);
    GAMENET_TEST_ASSERT(saved({RpcStatus::Ok, "closed"}) == RpcOperation::Closed);
}
void malformedAndBoundedContinuation() {
    RpcOptions options; options.maxFramesPerDispatch = 1;
    Fixture f(options, [](const auto&, auto reply) { (void)reply({RpcStatus::Ok, ""}); },
        {.maxPendingFunctors = 1, .reservedPendingFunctors = 0, .maxFunctorsPerIteration = 1});
    unsigned done = 0;
    (void)f.rpc->call(1, "", 1s, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::SchedulingRejected); ++done;
    });
    GAMENET_TEST_ASSERT(f.loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);
    f.feed(Fixture::wire({RpcMessageKind::Request, 1, 1, 1000, ""}) +
        Fixture::wire({RpcMessageKind::Request, 2, 1, 1000, ""}));
    GAMENET_TEST_ASSERT(done == 1 && f.rpc->snapshot().continuationRejections == 1);
}
void protocolAndThreadRules() {
    Fixture f;
    RpcCallResult foreign;
    RpcOperation cancel{}, close{};
    std::thread thread([&] {
        foreign = f.rpc->call(1, "", 1s, [](auto) {});
        cancel = f.rpc->cancel(1); close = f.rpc->close();
    }); thread.join();
    GAMENET_TEST_ASSERT(foreign.status == RpcOperation::WrongThread &&
        cancel == RpcOperation::WrongThread && close == RpcOperation::WrongThread);
    unsigned done = 0;
    (void)f.rpc->call(1, "", 1s, [&](RpcResult r) {
        GAMENET_TEST_ASSERT(r.status == RpcStatus::ProtocolError); ++done;
    });
    auto bad = Fixture::wire({RpcMessageKind::Request, 1, 1, 1000, ""});
    bad[4] = 'X'; f.feed(bad);
    GAMENET_TEST_ASSERT(done == 1 && f.rpc->snapshot().closed);
}
void continuationReentryAndRelease() {
    RpcOptions options; options.maxFramesPerDispatch = 1;
    std::shared_ptr<RpcChannel> channel;
    unsigned handlers = 0;
    RpcChannel::Reply weakReply;
    Fixture f(options, [&](const auto&, auto reply) {
        ++handlers;
        Buffer nested; nested.append(std::string_view("nested"));
        channel->onMessage(nested);
        GAMENET_TEST_ASSERT(nested.readableBytes() == 6);
        weakReply = reply;
        GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, "ok"}) == RpcOperation::Accepted);
        if (handlers == 2) (void)channel->close();
    });
    channel = f.rpc;
    f.feed(Fixture::wire({RpcMessageKind::Request, 1, 1, 1000, ""}) +
        Fixture::wire({RpcMessageKind::Request, 2, 1, 1000, ""}) +
        Fixture::wire({RpcMessageKind::Request, 3, 1, 1000, ""}));
    GAMENET_TEST_ASSERT(handlers == 1); // second frame runs on the next owner turn
    const auto progress = f.loop.runEvery(1ms, [&] {
        if (handlers == 2) { f.connection->forceClose(); f.loop.quit(); }
    });
    f.loop.runAfter(2s, [] { GAMENET_TEST_FAIL("RPC continuation did not make progress"); });
    f.loop.loop();
    f.loop.cancel(progress);
    GAMENET_TEST_ASSERT(handlers == 2 && channel->snapshot().reentrantMessages == 2 &&
        channel->snapshot().inboundRequests == 0);
    channel.reset(); f.rpc.reset();
    GAMENET_TEST_ASSERT(weakReply({RpcStatus::Ok, "expired"}) == RpcOperation::Closed);
}
void terminalCallbackReleasesCaller() {
    Fixture f;
    std::weak_ptr<RpcChannel> weak = f.rpc;
    unsigned done = 0;
    const auto admitted = f.rpc->call(1, "", 1s, [&](RpcResult result) {
        GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok);
        ++done; f.rpc.reset();
    });
    Buffer input;
    input.append(Fixture::wire({RpcMessageKind::Response, admitted.requestId, 0, 0, ""}));
    f.rpc->onMessage(input); // dispatch lease outlives the caller's final reference
    GAMENET_TEST_ASSERT(done == 1 && weak.expired());
}
void inboundByteLimitAndForeignReply() {
    RpcOptions options; options.maxInboundBytes = 28;
    RpcChannel::Reply reply;
    Fixture f(options, [&](const auto&, auto cb) { reply = cb; });
    f.feed(Fixture::wire({RpcMessageKind::Request, 1, 1, 1000, "x"}));
    GAMENET_TEST_ASSERT(f.rpc->snapshot().inboundRejected == 1 && !reply);
    f.feed(Fixture::wire({RpcMessageKind::Request, 2, 1, 1000, ""}));
    RpcOperation foreign{};
    std::thread worker([&] { foreign = reply({RpcStatus::Ok, ""}); }); worker.join();
    GAMENET_TEST_ASSERT(foreign == RpcOperation::WrongThread && f.rpc->snapshot().inboundRequests == 1);
    GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, ""}) == RpcOperation::Accepted);
    f.feed(Fixture::wire({RpcMessageKind::Request, 2, 1, 1000, ""}));
    GAMENET_TEST_ASSERT(f.rpc->snapshot().closed); // request ID replay is a protocol fault
}
void foreignReplyRacesOwnerRelease() {
    RpcChannel::Reply reply;
    Fixture f({}, [&](const auto&, auto value) { reply = std::move(value); });
    f.feed(Fixture::wire({RpcMessageKind::Request, 1, 1, 1000, ""}));
    std::atomic<bool> started{false}, stop{false};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            const auto result = reply({RpcStatus::Ok, ""});
            GAMENET_TEST_ASSERT(result == RpcOperation::WrongThread || result == RpcOperation::Closed);
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    f.rpc.reset(); // its weak foreign reply must not take over final ownership
    stop.store(true, std::memory_order_release);
    worker.join();
    GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, ""}) == RpcOperation::Closed);
}
}  // namespace
int main() {
    gamenet::test::verifyRpcStateSeeds();
    transportAdmission();
    limitsAndReentry(); byteLimitAndSendRejection(); sendRejection();
    deadlinesAndLateResponses(); disconnectAndDestructor(); shutdownAndWeakReply();
    inboundAndPartialFrames(); malformedAndBoundedContinuation(); protocolAndThreadRules();
    continuationReentryAndRelease(); terminalCallbackReleasesCaller(); inboundByteLimitAndForeignReply();
    foreignReplyRacesOwnerRelease();
}
