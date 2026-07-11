#include "gamenet/transport/TcpTransportEndpoint.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
    std::shared_ptr<gamenet::transport::TcpTransportEndpoint> endpoint;
    gamenet::net::EventLoopExecutor executor;

    {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair;
        auto connection = gamenet::test::makeTcpConnection(loop, pair, "transport-lifetime");
        endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
            gamenet::transport::TransportSessionId{77}, connection);
        executor = endpoint->ownerExecutor();
        GAMENET_TEST_ASSERT(executor.available());

        connection.reset();
        GAMENET_TEST_ASSERT(
            endpoint->send("expired-connection") == gamenet::transport::EndpointResult::Closed);
    }

    GAMENET_TEST_ASSERT(!executor.available());
    GAMENET_TEST_ASSERT(!executor.tryQueue([] {}));
    GAMENET_TEST_ASSERT(
        endpoint->send("expired-loop") == gamenet::transport::EndpointResult::Closed);
    GAMENET_TEST_ASSERT(
        endpoint->close(gamenet::transport::CloseReason::Normal) ==
        gamenet::transport::EndpointResult::Closed);
    GAMENET_TEST_ASSERT(!endpoint->isOpen());

    {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair pair;
        auto connection = gamenet::test::makeTcpConnection(loop, pair, "transport-final-drain");
        auto drainingEndpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
            gamenet::transport::TransportSessionId{78}, connection);
        connection->setCloseCallback(
            [](const gamenet::net::TcpConnectionPtr& closed) { closed->connectDestroyed(); });
        connection->connectEstablished();

        gamenet::transport::EndpointResult sendResult{};
        gamenet::transport::EndpointResult closeResult{};
        bool acceptedTaskRan = false;
        const auto drainingExecutor = drainingEndpoint->ownerExecutor();
        bool admitted = false;
        std::thread submitter([&] {
            admitted = drainingExecutor.tryQueue([&] {
                sendResult = drainingEndpoint->send("final-drain");
                closeResult = drainingEndpoint->close(
                    gamenet::transport::CloseReason::ProtocolError);
                acceptedTaskRan = true;
            });
        });
        submitter.join();
        GAMENET_TEST_ASSERT(admitted);

        loop.quit();
        loop.loop();

        GAMENET_TEST_ASSERT(acceptedTaskRan);
        GAMENET_TEST_ASSERT(sendResult == gamenet::transport::EndpointResult::Accepted);
        GAMENET_TEST_ASSERT(closeResult == gamenet::transport::EndpointResult::Accepted);
        GAMENET_TEST_ASSERT(!drainingExecutor.available());
        GAMENET_TEST_ASSERT(!drainingExecutor.tryQueue([] {}));
        GAMENET_TEST_ASSERT(!drainingEndpoint->isOpen());
        GAMENET_TEST_ASSERT(
            drainingEndpoint->send("after-final-drain") ==
            gamenet::transport::EndpointResult::OwnerUnavailable);
        GAMENET_TEST_ASSERT(
            drainingEndpoint->close(gamenet::transport::CloseReason::Normal) ==
            gamenet::transport::EndpointResult::OwnerUnavailable);
        bool rejectedAfterFinalDrain = false;
        try {
            gamenet::transport::TcpTransportEndpoint unavailableEndpoint(
                gamenet::transport::TransportSessionId{79}, connection);
            (void)unavailableEndpoint;
        } catch (const std::invalid_argument&) {
            rejectedAfterFinalDrain = true;
        }
        GAMENET_TEST_ASSERT(rejectedAfterFinalDrain);
        char received[32]{};
        const auto count = gamenet::net::sockets::read(pair.peerFd, received, sizeof(received));
        GAMENET_TEST_ASSERT(count == 11);
        GAMENET_TEST_ASSERT(std::string(received, static_cast<std::size_t>(count)) == "final-drain");
    }
}
