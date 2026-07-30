#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerId.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"

#ifdef _WIN32
#include "../../../src/core/net/platform/IocpTcpTransport.h"
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

int main() {
#ifndef _WIN32
    return 0;
#else
    constexpr std::size_t kWriteChunkBytes = 4096;
    // Stay below one physical write chunk while avoiding a dependency on
    // another process raising Windows' common ~15.6 ms timer resolution.
    constexpr std::size_t kPeerReadBytes = 2048;
    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    gamenet::net::detail::setIocpWriteChunkLimitForTesting(
        kWriteChunkBytes);

    std::string payload(256 * 1024, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>('A' + (index % 23));
    }

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(
        gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "iocp-partial-write-front-offset");
    connection->setBackpressureOptions(
        gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = payload.size() / 2,
            .highWaterMarkBytes = payload.size(),
            .hardLimitBytes = payload.size(),
        });

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(
        connection,
        loop,
        callbacks);

    std::string received;
    received.reserve(payload.size());
    gamenet::net::TcpSendResult sendResult{};
    int writeCompleteCallbacks = 0;
    bool closeIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenComplete = [&] {
        if (closeIssued ||
            writeCompleteCallbacks != 1 ||
            received.size() != payload.size()) {
            return;
        }
        closeIssued = true;
        loop.cancel(drainTimer);
        connection->forceClose();
    };

    connection->setWriteCompleteCallback(
        [&](const gamenet::net::TcpConnectionPtr&) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++writeCompleteCallbacks;
            closeWhenComplete();
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            loop.quit();
        });

    drainTimer = loop.runEvery(std::chrono::milliseconds(16), [&] {
        if (received.size() < payload.size()) {
            std::array<char, kPeerReadBytes> bytes{};
            const ssize_t n = gamenet::net::sockets::read(
                pair.peerFd,
                bytes.data(),
                bytes.size());
            if (n > 0) {
                received.append(
                    bytes.data(),
                    static_cast<std::size_t>(n));
                GAMENET_TEST_ASSERT(received.size() <= payload.size());
            } else {
                GAMENET_TEST_ASSERT(n != 0);
                const int error = gamenet::net::sockets::lastError();
                GAMENET_TEST_ASSERT(
                    gamenet::net::sockets::isWouldBlock(error) ||
                    gamenet::net::sockets::isInterrupted(error));
            }
        }
        closeWhenComplete();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        gamenet::test::runFromNonOwnerThread([&] {
            sendResult = connection->trySend(payload);
        });
        GAMENET_TEST_ASSERT(
            connection->pendingOutputBytes() == payload.size());
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(5),
        "partial IOCP write did not drain");

    GAMENET_TEST_ASSERT(
        sendResult == gamenet::net::TcpSendResult::Accepted);
    GAMENET_TEST_ASSERT(received == payload);
    GAMENET_TEST_ASSERT(writeCompleteCallbacks == 1);
    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::iocpWriteSubmissionCountForTesting() > 1);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpMaxWriteSubmissionBytesForTesting() <=
        kWriteChunkBytes);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPartialWriteCompletionCountForTesting() > 1);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPeakBufferedWriteBytesForTesting() ==
        payload.size());
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPeakWriteSegmentCountForTesting() == 1);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpCurrentBufferedWriteBytesForTesting() == 0);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpCurrentWriteSegmentCountForTesting() == 0);

    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    return 0;
#endif
}
