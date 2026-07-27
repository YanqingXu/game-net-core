#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpConnectionClose.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <chrono>
#include <optional>

namespace {

void testGracefulReasonWinsForcedEscalation() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "graceful-reason-wins-force-escalation");

    std::optional<gamenet::net::TcpConnectionCloseInfo> observed;
    connection->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            observed = info;
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            conn->connectDestroyed();
            loop.quit();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(
            connection->tryShutdown() ==
            gamenet::net::PostResult::Accepted);
        GAMENET_TEST_ASSERT(
            connection->tryForceClose() ==
            gamenet::net::PostResult::Accepted);
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "graceful close reason did not survive forced escalation");

    GAMENET_TEST_ASSERT(observed.has_value());
    GAMENET_TEST_ASSERT(
        observed->reason ==
        gamenet::net::TcpConnectionCloseReason::GracefulShutdown);
    GAMENET_TEST_ASSERT(
        connection->closeInfo() == observed);
}

void testPeerEofReason() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "peer-eof-close-reason");

    std::optional<gamenet::net::TcpConnectionCloseInfo> observed;
    connection->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            observed = info;
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn) {
            conn->connectDestroyed();
            loop.quit();
        });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        pair.closePeer();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "peer EOF close reason was not published");

    GAMENET_TEST_ASSERT(observed.has_value());
    GAMENET_TEST_ASSERT(
        observed->reason ==
        gamenet::net::TcpConnectionCloseReason::PeerEof);
}

}  // namespace

int main() {
    testGracefulReasonWinsForcedEscalation();
    testPeerEofReason();
    return 0;
}
