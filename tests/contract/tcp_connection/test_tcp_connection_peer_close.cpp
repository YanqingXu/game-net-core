#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include <chrono>
#include <memory>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "peer-close-converges-once");

    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int closeCallbackCount = 0;

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++connectedCallbackCount;
            return;
        }

        ++disconnectedCallbackCount;
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());
        pair.closePeer();
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for peer close teardown");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
