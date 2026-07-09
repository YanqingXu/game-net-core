#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <chrono>
#include <memory>

int main() {
    constexpr int iterationCount = 5;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair;
        auto connection = gamenet::test::makeTcpConnection(loop, pair, "cross-thread-force-close-soak-contract");

        gamenet::test::TcpConnectionCallbackCounts callbacks;
        gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);

        connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            connection.reset();
            loop.queueInLoop([&] { loop.quit(); });
        });

        loop.runAfter(std::chrono::milliseconds(0), [&] {
            auto conn = connection;
            conn->connectEstablished();
            GAMENET_TEST_ASSERT(conn->connected());

            // cross-thread-force-close-soak-contract: forceClose from a
            // non-owner thread must marshal back and keep teardown single-shot.
            gamenet::test::runFromNonOwnerThread([conn] {
                conn->forceClose();
            });
        });

        gamenet::test::runLoopWithTimeout(
            loop,
            std::chrono::seconds(1),
            "timed out waiting for cross-thread forceClose soak teardown");

        gamenet::test::assertSingleConnectDisconnectClose(callbacks);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
