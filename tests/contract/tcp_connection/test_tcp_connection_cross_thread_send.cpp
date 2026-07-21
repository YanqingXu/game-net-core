#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerId.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "cross-thread-send-marshals-to-owner-loop");

    const std::string payload = "cross-thread-send-payload";
    connection->setBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
        .lowWaterMarkBytes = payload.size() / 2,
        .highWaterMarkBytes = payload.size(),
        .hardLimitBytes = payload.size(),
    });

    std::size_t peerReadBytes = 0;
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int writeCompleteCallbackCount = 0;
    int closeCallbackCount = 0;
    bool sendIssued = false;
    gamenet::net::TcpSendResult saturatedResult{};
    bool closeIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenContractObserved = [&] {
        if (!closeIssued && peerReadBytes == payload.size() && writeCompleteCallbackCount == 1) {
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

        char buffer[64];
        while (peerReadBytes < payload.size()) {
            const ssize_t n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                peerReadBytes += static_cast<std::size_t>(n);
                GAMENET_TEST_ASSERT(peerReadBytes <= payload.size());
                continue;
            }
            if (n == 0) {
                GAMENET_TEST_ASSERT(false && "peer saw EOF before cross-thread send payload");
            }

            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error)) {
                closeWhenContractObserved();
                return;
            }

            GAMENET_TEST_ASSERT(false && "unexpected peer read error while draining cross-thread send payload");
        }
        closeWhenContractObserved();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());

        // cross-thread-send-marshals-to-owner-loop: send() from a non-owner
        // thread must copy the payload, marshal through EventLoop, and write
        // from the owner loop without executing user callbacks off-thread.
        gamenet::test::runFromNonOwnerThread([&] {
            conn->send(payload);
            saturatedResult = conn->trySend("x");
        });
        GAMENET_TEST_ASSERT(conn->pendingOutputBytes() == payload.size());
        sendIssued = true;
    });

    loop.runAfter(std::chrono::seconds(2), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for cross-thread send delivery");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(sendIssued);
    GAMENET_TEST_ASSERT(saturatedResult == gamenet::net::TcpSendResult::Overloaded);
    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1);
    GAMENET_TEST_ASSERT(peerReadBytes == payload.size());
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);

    return 0;
}
