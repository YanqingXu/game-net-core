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

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    std::shared_ptr<gamenet::net::TcpConnection> connection =
        gamenet::test::makeTcpConnection(loop, pair, "force-close-cancels-pending-read-before-destroy");

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
        conn->forceClose();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "timed out waiting for pending-read forceClose teardown");

    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(!connection);

    return 0;
}
