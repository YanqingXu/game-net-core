// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/TestAssert.h"
#include "support/RpcReconnectStress.h"
#include <any>
#include <thread>

using namespace std::chrono_literals;
using namespace gamenet::rpc;
namespace {
void existingRpcTcp() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "rpc-server");
    std::shared_ptr<RpcChannel> clientRpc;
    unsigned completed = 0, lateReplies = 0;
    bool disconnectStarted = false;
    const std::string binary("hello\0rpc", 9);
    server.setConnectionCallback([&](const auto& conn) {
        if (conn->connected()) {
            conn->setContext(RpcChannel::create(conn,
                [&, weakConnection = std::weak_ptr<gamenet::net::TcpConnection>(conn)](
                    const RpcMessage& message, auto reply) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                if (message.method == 1) {
                    GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, message.payload}) == RpcOperation::Accepted);
                } else if (message.method == 2) {
                    GAMENET_TEST_ASSERT(reply({RpcStatus::RemoteError, "unknown method"}) == RpcOperation::Accepted);
                } else if (message.method == 4) {
                    // Drop real TCP while the caller still owns a pending request.
                    if (const auto connection = weakConnection.lock()) connection->forceClose();
                } else {
                    loop.runAfter(120ms, [&, reply] {
                        GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, "late"}) == RpcOperation::NotFound);
                        ++lateReplies;
                    });
                }
            }));
        } else {
            auto channel = std::any_cast<std::shared_ptr<RpcChannel>>(conn->getContext());
            channel->onDisconnect();
            conn->setContext(std::any{});
        }
    });
    server.setMessageCallback([](const auto& conn, auto* input) {
        const auto channel = std::any_cast<std::shared_ptr<RpcChannel>>(conn->getContext());
        channel->onMessage(*input);
    });
    server.start();
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "rpc-client");
    client.setConnectionCallback([&](const auto& conn) {
        if (!conn->connected()) { if (clientRpc) clientRpc->onDisconnect(); return; }
        RpcOptions options; options.maxPendingRequests = 4;
        clientRpc = RpcChannel::create(conn, {}, options);
        auto check = [&](std::uint32_t method, std::string_view payload, auto timeout, RpcStatus expected) {
            return clientRpc->call(method, payload, timeout, [&, expected](RpcResult result) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread() && result.status == expected);
                if (result.status == RpcStatus::Ok) GAMENET_TEST_ASSERT(result.payload == binary);
                ++completed;
            });
        };
        GAMENET_TEST_ASSERT(check(1, binary, 2s, RpcStatus::Ok).status == RpcOperation::Accepted);
        GAMENET_TEST_ASSERT(check(2, "", 2s, RpcStatus::RemoteError).status == RpcOperation::Accepted);
        GAMENET_TEST_ASSERT(check(3, "", 50ms, RpcStatus::Timeout).status == RpcOperation::Accepted);
        const auto cancelled = check(3, "", 50ms, RpcStatus::Cancelled);
        GAMENET_TEST_ASSERT(cancelled.status == RpcOperation::Accepted);
        GAMENET_TEST_ASSERT(clientRpc->call(1, "", 1s, [](auto) {
            GAMENET_TEST_FAIL("rejected request invoked a callback");
        }).status == RpcOperation::PendingLimit);
        GAMENET_TEST_ASSERT(clientRpc->cancel(cancelled.requestId) == RpcOperation::Accepted);
        // A foreign caller uses the existing bounded executor; captured payload owns bytes.
        const auto executor = clientRpc->ownerExecutor();
        const auto channel = clientRpc;
        std::thread producer([&, executor, channel, payload = binary] {
            GAMENET_TEST_ASSERT(executor.post([&, channel, payload] {
                GAMENET_TEST_ASSERT(channel->call(1, payload, 2s, [&](RpcResult result) {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread() && result.status == RpcStatus::Ok && result.payload == binary);
                    ++completed;
                }).status == RpcOperation::Accepted);
            }) == gamenet::net::PostResult::Accepted);
        });
        producer.join();
    });
    client.setMessageCallback([&](const auto&, auto* input) { clientRpc->onMessage(*input); });
    const auto progress = loop.runEvery(2ms, [&] {
        if (completed == 5 && lateReplies == 2 && !disconnectStarted) {
            disconnectStarted = true;
            GAMENET_TEST_ASSERT(clientRpc->call(4, "disconnect", 2s, [&](RpcResult result) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread() && result.status == RpcStatus::Disconnected);
                ++completed;
            }).status == RpcOperation::Accepted);
        }
        if (completed != 6 || lateReplies != 2) return;
        const auto stats = clientRpc->snapshot();
        GAMENET_TEST_ASSERT(stats.acceptedCalls == 6 && stats.completedCalls == 6 &&
            stats.pendingRequests == 0 && stats.pendingBytes == 0);
        (void)clientRpc->close();
        if (auto connection = client.connection()) connection->forceClose();
        client.stop(); server.stop(); loop.quit();
    });
    client.connect();
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("RPC TCP integration timed out"); });
    loop.loop();
    loop.cancel(progress);
    GAMENET_TEST_ASSERT(completed == 6 && lateReplies == 2 && disconnectStarted);
}
} // namespace
int main() {
    existingRpcTcp();
    gamenet::test::runRpcReconnectStress(gamenet::test::plainRpcStressSession, "RPC-C3 TCP");
}
