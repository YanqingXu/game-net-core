#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpConnectionClose.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#ifdef _WIN32
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"
#include "../../../src/core/net/platform/IocpTcpTransport.h"
#endif

#include <chrono>
#include <memory>

int main() {
#ifdef _WIN32
    gamenet::net::detail::resetIocpTcpTransportFaultsForTesting();
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
#ifdef _WIN32
        GAMENET_TEST_ASSERT(
            gamenet::net::detail::EventLoopIocpAssociationHarness::tracks(
                loop,
                pair.connectionFd));
#endif
        GAMENET_TEST_ASSERT(
            connection->tryForceClose() ==
            gamenet::net::PostResult::Accepted);
#ifdef _WIN32
        GAMENET_TEST_ASSERT(connection->socketClosed());
        GAMENET_TEST_ASSERT(
            connection->closePhase() ==
            gamenet::net::TcpConnectionClosePhase::CompletionDraining);
        GAMENET_TEST_ASSERT(
            !gamenet::net::detail::EventLoopIocpAssociationHarness::tracks(
                loop,
                pair.connectionFd));
#endif
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
