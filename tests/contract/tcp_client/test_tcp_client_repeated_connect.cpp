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

namespace {

void testRepeatedConnectIsIdempotent() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-repeated-connect-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-repeated-connect-client");

    bool ownerConnectIssued = false;
    std::atomic<bool> nonOwnerConnectIssued{false};
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
            GAMENET_TEST_ASSERT(ownerConnectIssued);
            GAMENET_TEST_ASSERT(nonOwnerConnectIssued.load());
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
            GAMENET_TEST_ASSERT(ownerConnectIssued);
            GAMENET_TEST_ASSERT(nonOwnerConnectIssued.load());
            GAMENET_TEST_ASSERT(client.connection() == conn);

            // client-repeated-connect-is-idempotent: repeated owner and
            // non-owner connect() requests must not create multiple active
            // Connector attempts or TcpConnection objects.
            conn->forceClose();
            return;
        }

        ++clientDisconnectedCount;
        GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        maybeFinish();
    });

    server.start();

    ownerConnectIssued = true;
    client.connect();
    client.connect();

    gamenet::test::runFromNonOwnerThread([&] {
        GAMENET_TEST_ASSERT(!loop.isInLoopThread());
        nonOwnerConnectIssued = true;
        client.connect();
        client.connect();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        2s,
        "timed out waiting for repeated TcpClient connect teardown");

    GAMENET_TEST_ASSERT(ownerConnectIssued);
    GAMENET_TEST_ASSERT(nonOwnerConnectIssued.load());
    GAMENET_TEST_ASSERT(clientConnectedCount == 1);
    GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(serverConnectedCount == 1);
    GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

}

void testExplicitConnectAfterTerminalFailure() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "client-terminal-failure-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "client-terminal-failure-client");

    int clientConnectedCount = 0;
    int clientDisconnectedCount = 0;
    int serverConnectedCount = 0;
    int serverDisconnectedCount = 0;
    bool secondConnectIssued = false;
    bool finishQueued = false;

    auto maybeFinish = [&] {
        if (finishQueued || clientDisconnectedCount != 1 || serverDisconnectedCount != 1) {
            return;
        }
        finishQueued = true;
        loop.runAfter(50ms, [&] {
            server.stop();
            loop.quit();
        });
    };

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++serverConnectedCount;
            GAMENET_TEST_ASSERT(serverConnectedCount == 1);
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
            GAMENET_TEST_ASSERT(secondConnectIssued);
            GAMENET_TEST_ASSERT(clientConnectedCount == 1);
            conn->forceClose();
            return;
        }
        ++clientDisconnectedCount;
        GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        maybeFinish();
    });

    // client-connect-after-terminal-failure: the first no-retry attempt is
    // refused because the bound server socket is not listening. Its admitted
    // request must be released so the later explicit connect can succeed.
    client.connect();
    client.connect();
    loop.runAfter(250ms, [&] {
        server.start();
        secondConnectIssued = true;
        client.connect();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "timed out waiting for TcpClient connect after terminal failure");

    GAMENET_TEST_ASSERT(secondConnectIssued);
    GAMENET_TEST_ASSERT(clientConnectedCount == 1);
    GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(serverConnectedCount == 1);
    GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
}

}  // namespace

int main() {
    testRepeatedConnectIsIdempotent();
    testExplicitConnectAfterTerminalFailure();
    return 0;
}
