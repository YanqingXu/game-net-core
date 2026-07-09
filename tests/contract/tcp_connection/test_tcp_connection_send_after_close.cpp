#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "send-after-close-is-ignored");

    const std::string payload = "send-after-close-payload";

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);

    int writeCompleteCallbackCount = 0;
    std::size_t peerReadBytes = 0;
    bool ownerSendAfterCloseIssued = false;
    std::atomic<bool> nonOwnerSendAfterCloseIssued{false};

    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbackCount;
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++callbacks.closed;
        conn->connectDestroyed();

        // send-after-close-is-ignored: once a connection is disconnected,
        // owner and non-owner send() calls must be ignored rather than writing
        // to the peer or publishing write-complete callbacks.
        ownerSendAfterCloseIssued = true;
        conn->send(payload);
        gamenet::test::runFromNonOwnerThread([conn, &loop, &payload, &nonOwnerSendAfterCloseIssued] {
            GAMENET_TEST_ASSERT(!loop.isInLoopThread());
            nonOwnerSendAfterCloseIssued = true;
            conn->send(payload);
        });

        loop.runAfter(50ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());

            char buffer[64];
            const ssize_t n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                peerReadBytes += static_cast<std::size_t>(n);
            } else if (n < 0) {
                const int error = gamenet::net::sockets::lastError();
                GAMENET_TEST_ASSERT(
                    gamenet::net::sockets::isWouldBlock(error) ||
                    gamenet::net::sockets::isInterrupted(error));
            }

            GAMENET_TEST_ASSERT(ownerSendAfterCloseIssued);
            GAMENET_TEST_ASSERT(nonOwnerSendAfterCloseIssued.load());
            GAMENET_TEST_ASSERT(peerReadBytes == 0);
            GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 0);
            GAMENET_TEST_ASSERT(connection->disconnected());
            loop.quit();
        });
    });

    loop.runAfter(0ms, [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->forceClose();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "timed out waiting for send-after-close ignored contract");

    GAMENET_TEST_ASSERT(ownerSendAfterCloseIssued);
    GAMENET_TEST_ASSERT(nonOwnerSendAfterCloseIssued.load());
    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(peerReadBytes == 0);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 0);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
