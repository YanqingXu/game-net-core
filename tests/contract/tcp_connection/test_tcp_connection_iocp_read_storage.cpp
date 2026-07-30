#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/NetworkMemoryRetention.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerId.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#ifdef _WIN32
#include "../../../src/core/net/platform/IocpTcpTransport.h"
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

namespace {

#ifdef _WIN32
std::atomic<bool> observedReadCancellation{false};

void observeCompletion(
    gamenet::net::IocpOperationKind kind,
    int error) noexcept {
    if (kind == gamenet::net::IocpOperationKind::Read &&
        error == ERROR_OPERATION_ABORTED) {
        observedReadCancellation.store(true, std::memory_order_release);
    }
}
#endif

}  // namespace

int main() {
#ifndef _WIN32
    return 0;
#else
    using namespace std::chrono_literals;

    constexpr std::size_t kReadChunkBytes = 4 * 1024;
    const std::string payload(5 * kReadChunkBytes, 'r');

    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    gamenet::net::detail::setIocpCompletionObserverForTesting(
        &observeCompletion);
    observedReadCancellation.store(false, std::memory_order_relaxed);

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "iocp-bounded-on-demand-read-storage");
    connection->setBackpressureOptions(
        gamenet::net::TcpConnectionBackpressureOptions{
            .maxInputBufferBytes = kReadChunkBytes,
        });

    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpCurrentReadStorageBytesForTesting() == 0);
    {
        const auto retention =
            gamenet::net::networkFixedStorageRetentionSnapshot();
        GAMENET_TEST_ASSERT(retention.sharedReadPoolBytes == 0);
        GAMENET_TEST_ASSERT(retention.sharedReadSlabBytes == 0);
        GAMENET_TEST_ASSERT(retention.connectionLocalReadBytes == 0);
        GAMENET_TEST_ASSERT(
            retention.connectionLocalReadChunkLimitBytes ==
            kReadChunkBytes);
    }

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(
        connection,
        loop,
        callbacks);

    std::string received;
    received.reserve(payload.size());
    std::size_t sentBytes = 0;
    bool closeScheduled = false;
    gamenet::net::TimerId sendTimer;

    connection->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr&,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                input->readableBytes() <= kReadChunkBytes);
            received += input->retrieveAllAsString();
            GAMENET_TEST_ASSERT(received.size() <= payload.size());
            if (received.size() == payload.size() && !closeScheduled) {
                closeScheduled = true;
                loop.runAfter(1ms, [&] {
                    connection->forceClose();
                });
            }
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                observedReadCancellation.load(
                    std::memory_order_acquire));
            GAMENET_TEST_ASSERT(
                gamenet::net::detail::
                    iocpCurrentReadStorageBytesForTesting() == 0);
            GAMENET_TEST_ASSERT(
                gamenet::net::networkFixedStorageRetentionSnapshot().
                    connectionLocalReadBytes == 0);
            ++callbacks.closed;
            conn->connectDestroyed();
            loop.quit();
        });

    loop.runAfter(0ms, [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(
            gamenet::net::detail::iocpReadChunkBytesForTesting() ==
            kReadChunkBytes);
        GAMENET_TEST_ASSERT(
            gamenet::net::detail::
                iocpCurrentReadStorageBytesForTesting() ==
            kReadChunkBytes);
        {
            const auto retention =
                gamenet::net::networkFixedStorageRetentionSnapshot();
            GAMENET_TEST_ASSERT(
                retention.connectionLocalReadBytes ==
                kReadChunkBytes);
            GAMENET_TEST_ASSERT(
                retention.peakConnectionLocalReadBytes >=
                kReadChunkBytes);
            GAMENET_TEST_ASSERT(
                retention.totalRetainedBytes ==
                retention.acceptExFixedPoolBytes +
                    retention.iocpCompletionBatchBytes +
                    retention.connectionLocalReadBytes);
        }
        {
            const auto retention =
                connection->memoryRetentionSnapshot();
            GAMENET_TEST_ASSERT(
                retention.transportReadStorageBytes ==
                kReadChunkBytes);
            GAMENET_TEST_ASSERT(
                retention.peakTransportReadStorageBytes ==
                kReadChunkBytes);
        }

        sendTimer = loop.runEvery(1ms, [&] {
            while (sentBytes < payload.size()) {
                const ssize_t n = gamenet::net::sockets::write(
                    pair.peerFd,
                    payload.data() + sentBytes,
                    payload.size() - sentBytes);
                if (n > 0) {
                    sentBytes += static_cast<std::size_t>(n);
                    continue;
                }
                GAMENET_TEST_ASSERT(n < 0);
                const int error = gamenet::net::sockets::lastError();
                GAMENET_TEST_ASSERT(
                    gamenet::net::sockets::isWouldBlock(error) ||
                    gamenet::net::sockets::isInterrupted(error));
                return;
            }
            loop.cancel(sendTimer);
        });
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "bounded IOCP read storage did not drain and cancel");

    GAMENET_TEST_ASSERT(received == payload);
    GAMENET_TEST_ASSERT(sentBytes == payload.size());
    GAMENET_TEST_ASSERT(closeScheduled);
    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPeakReadStorageBytesForTesting() ==
        kReadChunkBytes);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpMaxReadSubmissionBytesForTesting() <=
        kReadChunkBytes);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpReadStorageAllocationCountForTesting() == 1);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpReadStorageReleaseCountForTesting() == 1);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpPositiveReadCompletionCountForTesting() >= 5);
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::
            iocpCurrentReadStorageBytesForTesting() == 0);
    {
        const auto retention =
            gamenet::net::networkFixedStorageRetentionSnapshot();
        GAMENET_TEST_ASSERT(retention.connectionLocalReadBytes == 0);
        GAMENET_TEST_ASSERT(
            retention.peakConnectionLocalReadBytes ==
            kReadChunkBytes);
    }
    {
        const auto retention =
            connection->memoryRetentionSnapshot();
        GAMENET_TEST_ASSERT(retention.transportReadStorageBytes == 0);
        GAMENET_TEST_ASSERT(
            retention.peakTransportReadStorageBytes ==
            kReadChunkBytes);
    }

    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    return 0;
#endif
}
