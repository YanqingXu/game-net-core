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
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-repeated-stop-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-repeated-stop-client");

    client.enableRetry();

    bool ownerStopIssued = false;
    std::atomic<bool> nonOwnerStopIssued{false};
    bool peerCloseQueued = false;
    bool finishQueued = false;
    int clientConnectedCount = 0;
    int clientDisconnectedCount = 0;
    int serverConnectedCount = 0;
    int serverDisconnectedCount = 0;
    gamenet::net::TcpConnectionPtr serverConnection;

    auto maybeClosePeer = [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (peerCloseQueued || !ownerStopIssued || !nonOwnerStopIssued.load() || !serverConnection) {
            return;
        }

        peerCloseQueued = true;
        loop.runAfter(50ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(ownerStopIssued);
            GAMENET_TEST_ASSERT(nonOwnerStopIssued.load());
            GAMENET_TEST_ASSERT(serverConnection);
            serverConnection->forceClose();
        });
    };

    auto maybeFinish = [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (finishQueued || clientDisconnectedCount != 1 || serverDisconnectedCount != 1) {
            return;
        }

        finishQueued = true;
        loop.runAfter(150ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(ownerStopIssued);
            GAMENET_TEST_ASSERT(nonOwnerStopIssued.load());
            GAMENET_TEST_ASSERT(peerCloseQueued);
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
            maybeClosePeer();
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

            // client-repeated-stop-idempotent: repeated owner and non-owner
            // stop() requests on an active retry-enabled client must clear
            // connect intent once, so peer close cannot resurrect the client.
            ownerStopIssued = true;
            client.stop();
            client.stop();
            gamenet::test::runFromNonOwnerThread([&] {
                GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                nonOwnerStopIssued = true;
                client.stop();
                client.stop();
            });
            maybeClosePeer();
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
        2s,
        "timed out waiting for repeated TcpClient stop teardown");

    GAMENET_TEST_ASSERT(ownerStopIssued);
    GAMENET_TEST_ASSERT(nonOwnerStopIssued.load());
    GAMENET_TEST_ASSERT(peerCloseQueued);
    GAMENET_TEST_ASSERT(clientConnectedCount == 1);
    GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(serverConnectedCount == 1);
    GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
