#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TestAssert.h"
#include <chrono>
#include <memory>
#include <string>
#include <thread>

int main() {
    constexpr int iterationCount = 5;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair;

        const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
        const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

        std::shared_ptr<gamenet::net::TcpConnection> connection = std::make_shared<gamenet::net::TcpConnection>(
            &loop,
            "cross-thread-force-close-pending-read-contract-" + std::to_string(iteration),
            pair.connectionFd,
            localAddr,
            peerAddr);

        gamenet::test::TcpConnectionCallbackCounts callbacks;
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

            // cross-thread-force-close-pending-read-contract: forceClose()
            // from a non-owner thread must marshal back to the owner loop and
            // cancel/drain pending read state before connection destruction.
            std::thread closer([&loop, conn] {
                GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                conn->forceClose();
            });
            closer.join();
        });

        gamenet::test::runLoopWithTimeout(
            loop,
            std::chrono::seconds(2),
            "timed out waiting for cross-thread pending-read forceClose teardown");

        gamenet::test::assertSingleConnectDisconnectClose(callbacks);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
