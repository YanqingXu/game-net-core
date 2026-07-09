#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "high-water-mark-fires-on-owner-loop");

    constexpr std::size_t highWaterMark = 1024;
    int highWaterCallbackCount = 0;
    int closeCallbackCount = 0;
    std::size_t highWaterBytes = 0;
    bool sendReturned = false;
    bool highWaterBeforeSendReturned = false;

    connection->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn, std::size_t bufferedBytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++highWaterCallbackCount;
            highWaterBytes = bufferedBytes;
            if (!sendReturned) {
                highWaterBeforeSendReturned = true;
            }
            conn->forceClose();
        },
        highWaterMark);

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    const std::string payload(4 * 1024 * 1024, 'h');
    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->send(payload);
        sendReturned = true;
        GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for high-water callback");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(sendReturned);
    GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    GAMENET_TEST_ASSERT(highWaterCallbackCount == 1);
    GAMENET_TEST_ASSERT(highWaterBytes >= highWaterMark);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
