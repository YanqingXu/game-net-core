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
#include <chrono>
#include <memory>
#include <string>

int main() {
    constexpr int iterationCount = 5;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);
        std::shared_ptr<gamenet::net::TcpConnection> connection = gamenet::test::makeTcpConnection(
            loop,
            pair,
            "force-close-pending-write-soak-contract-" + std::to_string(iteration));

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

            // force-close-pending-write-soak-contract: forceClose during a
            // large owner-loop write must cancel/drain pending IO before the
            // connection releases channel and operation storage.
            const std::string payload(8 * 1024 * 1024, 'w');
            conn->send(payload);
            conn->forceClose();
        });

        gamenet::test::runLoopWithTimeout(
            loop,
            std::chrono::seconds(2),
            "timed out waiting for pending-write forceClose soak teardown");

        gamenet::test::assertSingleConnectDisconnectClose(callbacks);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
