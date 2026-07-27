#include "support/TestAssert.h"

#ifdef _WIN32

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include "../../../src/core/net/platform/IocpTcpTransport.h"
#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

namespace {

std::atomic<int> observedReadCompletionError{0};

void observeCompletion(
    gamenet::net::IocpOperationKind kind,
    int error) noexcept {
    if (kind == gamenet::net::IocpOperationKind::Read && error != 0) {
        observedReadCompletionError.store(error, std::memory_order_relaxed);
    }
}

class ScopedIocpFaultHooks {
public:
    ScopedIocpFaultHooks() {
        gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
        gamenet::net::detail::setIocpCompletionObserverForTesting(
            &observeCompletion);
    }

    ~ScopedIocpFaultHooks() {
        gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    }
};

void runSynchronousReadFailureCase(int injectedError) {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "iocp-synchronous-read-failure");

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            loop.quit();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        gamenet::net::detail::
            injectNextIocpReadSubmissionErrorForTesting(injectedError);
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->disconnected());
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "synchronous WSARecv failure waited for a phantom completion");

    GAMENET_TEST_ASSERT(callbacks.connected == 0);
    GAMENET_TEST_ASSERT(callbacks.disconnected == 1);
    GAMENET_TEST_ASSERT(callbacks.closed == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
}

void runSynchronousWriteFailureAndRealCancelCase() {
    observedReadCompletionError.store(0, std::memory_order_relaxed);

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "iocp-synchronous-write-failure");

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            loop.quit();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());

        gamenet::net::detail::
            injectNextIocpWriteSubmissionErrorForTesting(WSAENOBUFS);
        const auto result = connection->trySend(std::string_view{"payload"});
        GAMENET_TEST_ASSERT(
            result == gamenet::net::TcpSendResult::Accepted);
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "synchronous WSASend failure did not drain the canceled read");

    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(
        observedReadCompletionError.load(std::memory_order_relaxed) ==
        ERROR_OPERATION_ABORTED);
}

}  // namespace

#endif

int main() {
#ifdef _WIN32
    ScopedIocpFaultHooks hooks;
    runSynchronousReadFailureCase(WSAENOBUFS);
    runSynchronousReadFailureCase(WSAECONNRESET);
    runSynchronousWriteFailureAndRealCancelCase();
#endif
    return 0;
}
