#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpConnectionClose.h"

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
#include <memory>

namespace {

#ifdef _WIN32
std::atomic<int> observedCancelCompletion{0};

void observeCompletion(
    gamenet::net::IocpOperationKind kind,
    int error) noexcept {
    if (kind == gamenet::net::IocpOperationKind::Read &&
        error == ERROR_OPERATION_ABORTED) {
        observedCancelCompletion.store(1, std::memory_order_release);
    }
}
#endif

}  // namespace

int main() {
#ifdef _WIN32
    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
    gamenet::net::detail::setIocpCompletionObserverForTesting(
        &observeCompletion);
    observedCancelCompletion.store(0, std::memory_order_relaxed);
#endif

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "explicit-socket-close-completion-drain");

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);

    int closeInfoCallbacks = 0;
    connection->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            ++closeInfoCallbacks;
            GAMENET_TEST_ASSERT(
                info.reason ==
                gamenet::net::TcpConnectionCloseReason::ForcedShutdown);
            GAMENET_TEST_ASSERT(conn->socketClosed());
            GAMENET_TEST_ASSERT(
                conn->closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
#ifdef _WIN32
            GAMENET_TEST_ASSERT(
                observedCancelCompletion.load(std::memory_order_acquire) == 1);
#endif
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            ++callbacks.closed;
            GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 1);
            conn->connectDestroyed();
            GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
            loop.quit();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 1);
        GAMENET_TEST_ASSERT(
            connection->tryForceClose() ==
            gamenet::net::PostResult::Accepted);
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "explicit socket close/completion drain did not converge");

    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(closeInfoCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->socketClosed());
    GAMENET_TEST_ASSERT(
        connection->closePhase() ==
        gamenet::net::TcpConnectionClosePhase::Closed);

#ifdef _WIN32
    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
#endif
    return 0;
}
