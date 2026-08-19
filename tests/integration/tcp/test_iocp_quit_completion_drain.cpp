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

namespace {

std::atomic<int> observedReadCompletionError{0};
std::atomic<gamenet::net::EventLoopPhase> observedReadCompletionPhase{
    gamenet::net::EventLoopPhase::Running};
gamenet::net::EventLoop* observedLoop{nullptr};

void observeCompletion(
    gamenet::net::IocpOperationKind kind,
    int error) noexcept {
    if (kind == gamenet::net::IocpOperationKind::Read && error != 0) {
        observedReadCompletionError.store(error, std::memory_order_relaxed);
        GAMENET_TEST_ASSERT(observedLoop != nullptr);
        observedReadCompletionPhase.store(
            observedLoop->phase(),
            std::memory_order_relaxed);
    }
}

class ScopedIocpCompletionObserver {
public:
    ScopedIocpCompletionObserver() {
        gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
        gamenet::net::detail::setIocpCompletionObserverForTesting(
            &observeCompletion);
    }

    ~ScopedIocpCompletionObserver() {
        gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    }
};

}  // namespace

#endif

int main() {
#ifdef _WIN32
    ScopedIocpCompletionObserver observer;
    observedReadCompletionError.store(0, std::memory_order_relaxed);
    observedReadCompletionPhase.store(
        gamenet::net::EventLoopPhase::Running,
        std::memory_order_relaxed);

    gamenet::net::EventLoop loop;
    observedLoop = &loop;
    gamenet::test::ConnectedSocketPair pair;
    std::shared_ptr<gamenet::net::TcpConnection> connection =
        gamenet::test::makeTcpConnection(
            loop,
            pair,
            "iocp-cancel-quit-completion-drain");

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++callbacks.closed;
            conn->connectDestroyed();
            connection.reset();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());

        // iocp-cancel-quit-completion-drain: forceClose requests cancellation
        // of the real pending WSARecv and quit seals admission in the same
        // owner-loop callback. EventLoop must keep zero-timeout polling until
        // ERROR_OPERATION_ABORTED is consumed and teardown becomes silent.
        connection->forceClose();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "EventLoop returned before canceled IOCP completion drain");

    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(!connection);
    GAMENET_TEST_ASSERT(
        observedReadCompletionError.load(std::memory_order_relaxed) ==
        ERROR_OPERATION_ABORTED);
    GAMENET_TEST_ASSERT(
        observedReadCompletionPhase.load(std::memory_order_relaxed) ==
        gamenet::net::EventLoopPhase::Quiescing);
    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);
    observedLoop = nullptr;
#endif
    return 0;
}
