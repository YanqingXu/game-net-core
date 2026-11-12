// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/TestAssert.h"
#include <any>
#include <array>
#include <cstdio>
#include <vector>

namespace gamenet::test {
struct RpcStressSession {
    std::shared_ptr<rpc::RpcChannel> channel;
    std::shared_ptr<void> lifetime;
    std::function<void()> start, down, writable, settled;
    std::function<void(net::Buffer&)> feed;
};
inline std::shared_ptr<RpcStressSession> plainRpcStressSession(
    const net::TcpConnectionPtr& connection, rpc::RpcChannel::Handler handler,
    std::function<void()> ready, std::function<void()> closed, bool) {
    auto result = std::make_shared<RpcStressSession>();
    result->channel = rpc::RpcChannel::create(connection, std::move(handler));
    result->lifetime = result->channel;
    result->start = std::move(ready);
    result->feed = [c = result->channel](auto& input) { c->onMessage(input); };
    result->down = [c = result->channel, closed = std::move(closed)] { c->onDisconnect(); closed(); };
    result->writable = [] {}; result->settled = [] {};
    return result;
}

// Factory composes either ordinary RPC or real OpenSSL RPC/TLS. Test-owned held
// input is finite and always consumed before disconnect/next generation.
template<class Factory> void runRpcReconnectStress(Factory factory, const char* label) {
    using namespace std::chrono_literals;
    using namespace rpc;
    net::EventLoop loop;
    net::TcpServer server(&loop, net::InetAddress(0, true), "rpc-c3-stress-server");
    server.setConnectionBackpressureOptions({.lowWaterMarkBytes = 256 * 1024,
        .highWaterMarkBytes = 512 * 1024, .hardLimitBytes = 1024 * 1024});
    constexpr unsigned generations = 8, burst = 128;
    enum class Phase { Connect, Burst, Recover, Probe, Deferred, Cancelled, Drop, Retire };
    Phase phase = Phase::Connect;
    unsigned generation = 0, serverReady = 0, clientReady = 0, serverDown = 0, clientDown = 0;
    unsigned serverClosed = 0, clientClosed = 0, admitted = 0, completed = 0, rejected = 0, totalRejected = 0;
    unsigned deferred = 0;
    struct Call { bool accepted{}; unsigned done{}; std::size_t bytes{}; RpcStatus expected{RpcStatus::Ok}; };
    std::array<Call, burst + 4> ledger{};
    unsigned issued = 0;
    std::uint64_t deferredId = 0;
    bool hold = false, probeDone = false;
    net::Buffer held;
    std::vector<RpcChannel::Reply> savedReplies;
    std::vector<RpcChannel::Reply> oldReplies;
    std::vector<std::weak_ptr<void>> weak;
    std::shared_ptr<RpcStressSession> clientSession, serverSession;
    net::TcpConnectionPtr clientConnection, serverConnection;
    std::unique_ptr<net::TcpClient> client;
    auto context = [](const auto& connection) {
        const auto* p = std::any_cast<std::shared_ptr<RpcStressSession>>(&connection->getContext());
        return p ? *p : nullptr;
    };
    auto verify = [&] {
        if (!clientSession) return;
        const auto s = clientSession->channel->snapshot();
        unsigned pending = 0, done = 0, accepted = 0; std::size_t bytes = 0;
        for (unsigned i = 0; i < issued; ++i) {
            const auto& c = ledger[i];
            GAMENET_TEST_ASSERT(c.done <= (c.accepted ? 1u : 0u));
            if (c.accepted) { ++accepted; if (c.done) ++done; else { ++pending; bytes += c.bytes; } }
        }
        GAMENET_TEST_ASSERT(s.acceptedCalls == accepted && s.completedCalls == done &&
            s.pendingRequests == pending && s.pendingBytes == bytes && admitted == accepted && completed == done);
        GAMENET_TEST_ASSERT(clientConnection->pendingOutputBytes() <= 8192);
    };
    auto call = [&](std::uint32_t method, std::string payload, RpcStatus expected) {
        GAMENET_TEST_ASSERT(issued < ledger.size());
        const auto index = issued++; ledger[index].bytes = payload.size() + 28; ledger[index].expected = expected;
        const auto currentGeneration = generation;
        const auto result = clientSession->channel->call(method, payload, 5s,
            [&, index, currentGeneration, payload](RpcResult result) {
                auto& c = ledger[index];
                GAMENET_TEST_ASSERT(generation == currentGeneration && c.accepted && ++c.done == 1 &&
                    result.status == c.expected && loop.isInLoopThread());
                if (result.status == RpcStatus::Ok) GAMENET_TEST_ASSERT(result.payload == payload);
                ++completed;
            });
        // Call argument payload must remain valid until admission returns.
        ledger[index].accepted = result.status == RpcOperation::Accepted;
        if (ledger[index].accepted) ++admitted;
        else {
            GAMENET_TEST_ASSERT(result.status == RpcOperation::SendRejected && result.requestId == 0);
            ++rejected;
        }
        return result;
    };
    auto wire = [&](auto& endpoint, bool isServer) {
        endpoint.setMessageCallback([&, isServer](const auto& connection, auto* input) {
            if (isServer && hold) {
                GAMENET_TEST_ASSERT(held.readableBytes() + input->readableBytes() <= 1024 * 1024);
                held.append(std::string_view(input->peek(), input->readableBytes())); input->retrieveAll();
            } else if (auto s = context(connection)) s->feed(*input); else input->retrieveAll();
        });
        endpoint.setWriteCompleteCallback([&](const auto& connection) { if (auto s = context(connection)) s->writable(); });
        endpoint.setCallbackExceptionHandler([](const auto&, const auto&) { GAMENET_TEST_FAIL("stress callback escaped into Core"); });
    };
    wire(server, true);
    server.setConnectionCallback([&](const auto& connection) {
        if (!connection->connected()) {
            if (auto s = context(connection)) s->down(); connection->setContext(std::any{}); ++serverDown; return;
        }
        serverConnection = connection;
        serverSession = factory(connection, [&](const RpcMessage& request, auto reply) {
            if (request.method == 1) GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, request.payload}) == RpcOperation::Accepted);
            else { ++deferred; savedReplies.push_back(std::move(reply)); }
        }, [&] { ++serverReady; }, [&] { ++serverClosed; }, false);
        connection->setContext(serverSession); serverSession->start();
    });
    server.start();
    auto connect = [&] {
        client = std::make_unique<net::TcpClient>(&loop, server.listenAddress(), "rpc-c3-stress-client");
        client->setConnectionBackpressureOptions({.lowWaterMarkBytes = 2048, .highWaterMarkBytes = 4096, .hardLimitBytes = 8192});
        wire(*client, false);
        client->setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) {
                if (auto s = context(connection)) s->down(); connection->setContext(std::any{}); ++clientDown; return;
            }
            connection->setSendBufferSize(4096);
            clientConnection = connection;
            clientSession = factory(connection, {}, [&] { ++clientReady; }, [&] { ++clientClosed; }, true);
            connection->setContext(clientSession); clientSession->start();
        });
        client->connect();
    };
    connect();
    loop.runEvery(1ms, [&] {
        verify();
        switch (phase) {
        case Phase::Connect:
            if (!clientReady || !serverReady) break;
            GAMENET_TEST_ASSERT(clientReady == 1 && serverReady == 1);
            for (const auto& reply : oldReplies)
                GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, "prior-generation"}) == RpcOperation::Closed);
            GAMENET_TEST_ASSERT(clientSession->channel->snapshot().acceptedCalls == 0);
            hold = true;
            for (unsigned n = 0; n < burst; ++n) (void)call(1, std::string(4096, static_cast<char>('a' + generation)), RpcStatus::Ok);
            GAMENET_TEST_ASSERT(admitted && rejected && !completed);
            totalRejected += rejected; phase = Phase::Burst; break;
        case Phase::Burst:
            if (!held.readableBytes()) break;
            hold = false; serverSession->feed(held);
            GAMENET_TEST_ASSERT(!held.readableBytes()); phase = Phase::Recover; break;
        case Phase::Recover:
            if (completed != admitted) break;
            GAMENET_TEST_ASSERT(!clientSession->channel->snapshot().pendingBytes);
            GAMENET_TEST_ASSERT(call(1, "probe-" + std::to_string(generation), RpcStatus::Ok).status == RpcOperation::Accepted);
            phase = Phase::Probe; break;
        case Phase::Probe:
            if (completed != admitted) break;
            probeDone = true;
            deferredId = call(2, "deferred", generation % 2 ? RpcStatus::Disconnected : RpcStatus::Cancelled).requestId;
            GAMENET_TEST_ASSERT(deferredId); phase = Phase::Deferred; break;
        case Phase::Deferred:
            if (!deferred) break;
            if (generation % 2 == 0) {
                GAMENET_TEST_ASSERT(clientSession->channel->cancel(deferredId) == RpcOperation::Accepted);
                GAMENET_TEST_ASSERT(clientSession->channel->cancel(deferredId) == RpcOperation::NotFound);
                GAMENET_TEST_ASSERT(savedReplies.back()({RpcStatus::Ok, "late"}) == RpcOperation::Accepted);
                GAMENET_TEST_ASSERT(savedReplies.back()({RpcStatus::Ok, "duplicate"}) == RpcOperation::NotFound);
                phase = Phase::Cancelled;
            } else { serverConnection->forceClose(); phase = Phase::Drop; }
            break;
        case Phase::Cancelled:
            if (!clientSession->channel->snapshot().ignoredResponses) break;
            GAMENET_TEST_ASSERT(call(2, "pending-at-drop", RpcStatus::Disconnected).status == RpcOperation::Accepted);
            phase = Phase::Drop; break;
        case Phase::Drop:
            if (generation % 2 == 0 && !clientDown) {
                if (deferred < 2) break;
                clientConnection->forceClose();
            }
            if (!clientDown || !serverDown || !clientClosed || !serverClosed) break;
            verify();
            GAMENET_TEST_ASSERT(clientDown == 1 && serverDown == 1 && clientClosed == 1 && serverClosed == 1 &&
                probeDone && admitted == completed && !held.readableBytes());
            for (const auto& s : {clientSession, serverSession}) {
                const auto snap = s->channel->snapshot();
                GAMENET_TEST_ASSERT(snap.closed && !snap.pendingRequests && !snap.pendingBytes && !snap.inboundRequests && !snap.inboundBytes);
                s->settled(); weak.push_back(s->channel); weak.push_back(s->lifetime);
            }
            for (const auto& reply : savedReplies) GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, "old-generation"}) == RpcOperation::Closed);
            oldReplies.insert(oldReplies.end(), savedReplies.begin(), savedReplies.end());
            GAMENET_TEST_ASSERT(!clientConnection->pendingOutputBytes() && !serverConnection->pendingOutputBytes());
            weak.push_back(clientConnection); weak.push_back(serverConnection);
            clientSession.reset(); serverSession.reset(); clientConnection.reset(); serverConnection.reset();
            client->stop(); client.reset(); savedReplies.clear(); phase = Phase::Retire; break;
        case Phase::Retire:
            for (const auto& value : weak) if (!value.expired()) return;
            weak.clear();
            if (++generation == generations) { server.stop(); loop.quit(); break; }
            serverReady = clientReady = serverDown = clientDown = serverClosed = clientClosed = 0;
            admitted = completed = rejected = issued = deferred = 0; ledger = {}; probeDone = false;
            phase = Phase::Connect; connect(); break;
        }
    });
    loop.runAfter(15s, [&] {
        std::fprintf(stderr, "%s generation=%u phase=%d admitted=%u completed=%u rejected=%u down=%u/%u closed=%u/%u held=%zu\n",
            label, generation, static_cast<int>(phase), admitted, completed, rejected, clientDown, serverDown, clientClosed, serverClosed, held.readableBytes());
        GAMENET_TEST_FAIL("RPC-C3 reconnect stress timed out");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(generation == generations && totalRejected >= generations && weak.empty());
    std::fprintf(stderr, "%s: %u generations, %u bounded send rejections, recovery/probe and zero-residue teardown passed\n",
        label, generation, totalRejected);
}
} // namespace gamenet::test
