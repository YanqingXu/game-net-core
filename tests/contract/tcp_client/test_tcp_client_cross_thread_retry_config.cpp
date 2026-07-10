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
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-retry-config-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-retry-config-client");

    client.enableRetry();

    std::atomic<bool> nonOwnerDisableIssued{false};
    bool peerCloseQueued = false;
    bool finishQueued = false;
    int clientConnectedCount = 0;
    int clientDisconnectedCount = 0;
    int serverConnectedCount = 0;
    int serverDisconnectedCount = 0;
    gamenet::net::TcpConnectionPtr serverConnection;

    auto maybeFinish = [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (finishQueued || clientDisconnectedCount != 1 || serverDisconnectedCount != 1) {
            return;
        }

        finishQueued = true;
        loop.runAfter(700ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(nonOwnerDisableIssued.load());
            GAMENET_TEST_ASSERT(peerCloseQueued);
            GAMENET_TEST_ASSERT(!client.retry());
            GAMENET_TEST_ASSERT(clientConnectedCount == 1);
            GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
            GAMENET_TEST_ASSERT(serverConnectedCount == 1);
            GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
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
            serverConnection = conn;
            return;
        }

        ++serverDisconnectedCount;
        GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
        serverConnection.reset();
        maybeFinish();
    });

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++clientConnectedCount;
            GAMENET_TEST_ASSERT(clientConnectedCount == 1);
            GAMENET_TEST_ASSERT(client.connection() == conn);

            // client-cross-thread-retry-config: non-owner disableRetry()
            // must marshal retry state mutation to the owner loop so a later
            // peer close does not resurrect the client through reconnect.
            gamenet::test::runFromNonOwnerThread([&] {
                GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                nonOwnerDisableIssued = true;
                client.disableRetry();
            });

            peerCloseQueued = true;
            loop.runAfter(50ms, [&] {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(nonOwnerDisableIssued.load());
                GAMENET_TEST_ASSERT(!client.retry());
                GAMENET_TEST_ASSERT(serverConnection);
                serverConnection->forceClose();
            });
            return;
        }

        ++clientDisconnectedCount;
        GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        maybeFinish();
    });

    server.start();
    client.connect();

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "timed out waiting for cross-thread TcpClient retry config contract");

    GAMENET_TEST_ASSERT(nonOwnerDisableIssued.load());
    GAMENET_TEST_ASSERT(peerCloseQueued);
    GAMENET_TEST_ASSERT(!client.retry());
    GAMENET_TEST_ASSERT(clientConnectedCount == 1);
    GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(serverConnectedCount == 1);
    GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
