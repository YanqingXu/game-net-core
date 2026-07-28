#include "gamenet/transport/TcpTransportEndpoint.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <atomic>
#include <thread>

int main() {
    gamenet::net::EventLoop loop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::test::ConnectedSocketPair pair;
    auto connection =
        gamenet::test::makeTcpConnection(loop, pair, "transport-control-close");
    auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
        gamenet::transport::TransportSessionId{91}, connection);
    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& closed) {
        closed->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();

    GAMENET_TEST_ASSERT(
        loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        loop.executor().post([] {}) == gamenet::net::PostResult::QueueFull);

    gamenet::DispatchResult result = gamenet::DispatchResult::OwnerUnavailable;
    std::thread closer([&] {
        result = endpoint->requestClose(gamenet::transport::CloseReason::Overloaded);
    });
    closer.join();
    GAMENET_TEST_ASSERT(result == gamenet::DispatchResult::Accepted);

    loop.loop();
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(
        endpoint->requestClose(gamenet::transport::CloseReason::Normal) ==
        gamenet::DispatchResult::EndpointClosed);
}
