#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerId.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"

#include <chrono>
#include <cstddef>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "repeated-shutdown-drains-once");

    const std::string payload(2 * 1024 * 1024, 'r');

    std::size_t peerReadBytes = 0;
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int writeCompleteCallbackCount = 0;
    int closeCallbackCount = 0;
    bool ownerShutdownIssued = false;
    bool nonOwnerShutdownIssued = false;
    bool peerSawEof = false;
    bool closeIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenContractObserved = [&] {
        if (!closeIssued && peerSawEof && writeCompleteCallbackCount == 1) {
            closeIssued = true;
            loop.cancel(drainTimer);
            connection->forceClose();
        }
    };

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++connectedCallbackCount;
            return;
        }
        ++disconnectedCallbackCount;
    });

    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbackCount;
        closeWhenContractObserved();
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    drainTimer = loop.runEvery(std::chrono::milliseconds(1), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        char buffer[8192];
        while (true) {
            const ssize_t n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                peerReadBytes += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                peerSawEof = true;
                closeWhenContractObserved();
                return;
            }

            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error)) {
                return;
            }

            GAMENET_TEST_ASSERT(false && "unexpected peer read error while draining repeated shutdown output");
        }
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->send(payload);

        // repeated-shutdown-drains-once: repeated owner and non-owner
        // shutdown() calls must converge on one owner-loop half-close after
        // pending output drains.
        conn->shutdown();
        conn->shutdown();
        ownerShutdownIssued = true;

        gamenet::test::runFromNonOwnerThread([conn] {
            conn->shutdown();
            conn->shutdown();
        });
        nonOwnerShutdownIssued = true;
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(2),
        "timed out waiting for repeated shutdown output drain");

    GAMENET_TEST_ASSERT(ownerShutdownIssued);
    GAMENET_TEST_ASSERT(nonOwnerShutdownIssued);
    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1);
    GAMENET_TEST_ASSERT(peerReadBytes == payload.size());
    GAMENET_TEST_ASSERT(peerSawEof);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
