// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-cross-thread-connect-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-cross-thread-connect-client");

    std::atomic<bool> connectIssued{false};
    int clientConnectedCount = 0;
    int clientDisconnectedCount = 0;
    int serverConnectedCount = 0;
    int serverDisconnectedCount = 0;
    bool finishQueued = false;

    auto maybeFinish = [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (finishQueued || clientDisconnectedCount != 1 || serverDisconnectedCount != 1) {
            return;
        }

        finishQueued = true;
        loop.runAfter(50ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(connectIssued.load());
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            server.stop();
            loop.quit();
        });
    };

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++serverConnectedCount;
            GAMENET_TEST_ASSERT(serverConnectedCount == 1);
            GAMENET_TEST_ASSERT(server.connectionCount() == 1);
            return;
        }

        ++serverDisconnectedCount;
        GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
        maybeFinish();
    });

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++clientConnectedCount;
            GAMENET_TEST_ASSERT(clientConnectedCount == 1);
            GAMENET_TEST_ASSERT(connectIssued.load());
            GAMENET_TEST_ASSERT(client.connection() == conn);

            // client-cross-thread-connect: non-owner connect() must marshal
            // Connector startup to the owner loop and publish callbacks there.
            conn->forceClose();
            return;
        }

        ++clientDisconnectedCount;
        GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        maybeFinish();
    });

    server.start();

    gamenet::test::runFromNonOwnerThread([&] {
        GAMENET_TEST_ASSERT(!loop.isInLoopThread());
        connectIssued = true;
        client.connect();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        2s,
        "timed out waiting for cross-thread TcpClient connect teardown");

    GAMENET_TEST_ASSERT(connectIssued.load());
    GAMENET_TEST_ASSERT(clientConnectedCount == 1);
    GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(serverConnectedCount == 1);
    GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
