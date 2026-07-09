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
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

int main() {
    constexpr int iterationCount = 8;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair;
        std::shared_ptr<gamenet::net::TcpConnection> connection = gamenet::test::makeTcpConnection(
            loop,
            pair,
            "force-close-pending-read-mixed-timing-soak-" + std::to_string(iteration));

        gamenet::test::TcpConnectionCallbackCounts callbacks;
        std::atomic<bool> forceCloseIssued{false};
        std::atomic<bool> ownerForceCloseIssued{false};
        std::atomic<bool> nonOwnerForceCloseIssued{false};
        gamenet::test::NonOwnerThreadHandoff closer;

        gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);

        connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            connection.reset();
            loop.runAfter(std::chrono::milliseconds(50), [&] { loop.quit(); });
        });

        loop.runAfter(std::chrono::milliseconds(0), [&] {
            auto conn = connection;
            conn->connectEstablished();
            GAMENET_TEST_ASSERT(conn->connected());

            // force-close-pending-read-mixed-timing-soak: immediate and
            // delayed owner/non-owner forceClose() calls must converge on one
            // owner-loop cancel/close path while a read may be pending.
            const int mode = iteration % 4;
            if (mode == 0) {
                forceCloseIssued = true;
                ownerForceCloseIssued = true;
                conn->forceClose();
            } else if (mode == 1) {
                closer = gamenet::test::startNonOwnerThread([conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued] {
                    GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                    forceCloseIssued = true;
                    nonOwnerForceCloseIssued = true;
                    conn->forceClose();
                });
            } else if (mode == 2) {
                loop.runAfter(
                    std::chrono::milliseconds(5 + iteration),
                    [conn, &loop, &forceCloseIssued, &ownerForceCloseIssued] {
                        GAMENET_TEST_ASSERT(loop.isInLoopThread());
                        forceCloseIssued = true;
                        ownerForceCloseIssued = true;
                        conn->forceClose();
                    });
            } else {
                closer = gamenet::test::startNonOwnerThreadAfter(
                    std::chrono::milliseconds(5 + iteration),
                    [conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued] {
                    GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                    forceCloseIssued = true;
                    nonOwnerForceCloseIssued = true;
                    conn->forceClose();
                });
            }
        });

        gamenet::test::runLoopWithTimeout(
            loop,
            std::chrono::seconds(2),
            "timed out waiting for mixed-timing pending-read forceClose teardown");

        closer.join();

        GAMENET_TEST_ASSERT(forceCloseIssued.load());
        GAMENET_TEST_ASSERT(ownerForceCloseIssued.load() || nonOwnerForceCloseIssued.load());
        gamenet::test::assertSingleConnectDisconnectClose(callbacks);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
