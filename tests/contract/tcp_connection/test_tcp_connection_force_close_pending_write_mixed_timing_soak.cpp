#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

int main() {
    constexpr int iterationCount = 8;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);

        const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
        const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

        std::shared_ptr<gamenet::net::TcpConnection> connection = std::make_shared<gamenet::net::TcpConnection>(
            &loop,
            "force-close-pending-write-mixed-timing-soak-" + std::to_string(iteration),
            pair.connectionFd,
            localAddr,
            peerAddr);

        gamenet::test::TcpConnectionCallbackCounts callbacks;
        std::atomic<bool> forceCloseIssued{false};
        std::atomic<bool> ownerForceCloseIssued{false};
        std::atomic<bool> nonOwnerForceCloseIssued{false};
        std::thread closer;

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

            const std::string payload(8 * 1024 * 1024, 'm');
            conn->send(payload);

            // force-close-pending-write-mixed-timing-soak: immediate and
            // delayed owner/non-owner forceClose() calls must converge on one
            // owner-loop cancel/close path while a large write may be pending.
            const int mode = iteration % 4;
            if (mode == 0) {
                forceCloseIssued = true;
                ownerForceCloseIssued = true;
                conn->forceClose();
            } else if (mode == 1) {
                closer = std::thread([conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued] {
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
                closer = std::thread([conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued, iteration] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5 + iteration));
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
            "timed out waiting for mixed-timing pending-write forceClose teardown");

        if (closer.joinable()) {
            closer.join();
        }

        GAMENET_TEST_ASSERT(forceCloseIssued.load());
        GAMENET_TEST_ASSERT(ownerForceCloseIssued.load() || nonOwnerForceCloseIssued.load());
        gamenet::test::assertSingleConnectDisconnectClose(callbacks);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
