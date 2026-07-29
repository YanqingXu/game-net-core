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
    constexpr std::size_t kWriteChunkBytes = 1024;
    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    gamenet::net::detail::setIocpWriteChunkLimitForTesting(
        kWriteChunkBytes);

    const std::array<std::string, 3> payloads{
        std::string(48 * 1024, 'a'),
        std::string(32 * 1024, 'b'),
        std::string(16 * 1024, 'c'),
    };
    std::string expected;
    for (const auto& payload : payloads) {
        expected += payload;
    }

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(
        gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "iocp-stable-segment-write");
    connection->setBackpressureOptions(
        gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = expected.size() / 2,
            .highWaterMarkBytes = expected.size(),
            .hardLimitBytes = expected.size(),
        });

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(
        connection,
        loop,
        callbacks);

    std::string received;
    received.reserve(expected.size());
    std::array<gamenet::net::TcpSendResult, 3> sendResults{};
    int writeCompleteCallbacks = 0;
    bool closeIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenComplete = [&] {
        if (closeIssued ||
            writeCompleteCallbacks != 1 ||
            received.size() != expected.size()) {
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

    drainTimer = loop.runEvery(std::chrono::milliseconds(1), [&] {
        std::array<char, 4096> bytes{};
        while (received.size() < expected.size()) {
            const ssize_t n = gamenet::net::sockets::read(
                pair.peerFd,
                bytes.data(),
                bytes.size());
            if (n > 0) {
                received.append(
                    bytes.data(),
                    static_cast<std::size_t>(n));
                GAMENET_TEST_ASSERT(received.size() <= expected.size());
                continue;
            }
            GAMENET_TEST_ASSERT(n != 0);
            const int error = gamenet::net::sockets::lastError();
            GAMENET_TEST_ASSERT(
                gamenet::net::sockets::isWouldBlock(error) ||
                gamenet::net::sockets::isInterrupted(error));
            break;
        }
        closeWhenComplete();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        gamenet::test::runFromNonOwnerThread([&] {
            for (std::size_t index = 0;
                 index < payloads.size();
                 ++index) {
                sendResults[index] =
                    connection->trySend(payloads[index]);
            }
        });
        GAMENET_TEST_ASSERT(
            connection->pendingOutputBytes() == expected.size());
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(5),
        "segmented IOCP write did not drain");

    for (const auto result : sendResults) {
        GAMENET_TEST_ASSERT(
            result == gamenet::net::TcpSendResult::Accepted);
    }
    GAMENET_TEST_ASSERT(received == expected);
    GAMENET_TEST_ASSERT(writeCompleteCallbacks == 1);
    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::iocpWriteSubmissionCountForTesting() >
        payloads.size());
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpMaxWriteSubmissionBytesForTesting() <=
        kWriteChunkBytes);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPartialWriteCompletionCountForTesting() > 0);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPeakBufferedWriteBytesForTesting() ==
        expected.size());
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPeakWriteSegmentCountForTesting() ==
        payloads.size());
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
