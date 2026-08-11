#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include "../../../src/core/net/detail/TcpConnectionConstructionHarness.h"
#include <chrono>
#include <memory>
#include <vector>

using namespace std::chrono_literals;

namespace {

void verifyConstructionFailureReleasesRequestBeforeNotification() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    std::vector<gamenet::net::SocketFd> acceptedSockets;
    acceptor.setNewConnectionCallback(
        [&](gamenet::net::SocketFd fd, const gamenet::net::InetAddress&) {
            acceptedSockets.push_back(fd);
        });
    acceptor.listen();

    gamenet::net::TcpClient client(
        &loop,
        acceptor.listenAddress(),
        "client-construction-failure-contract");
    int connectedCallbacks = 0;
    int disconnectedCallbacks = 0;
    int terminalFailures = 0;
    bool reconnectAccepted = false;
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                ++connectedCallbacks;
                connection->forceClose();
            } else {
                ++disconnectedCallbacks;
            }
        });
    client.setTerminalConnectFailureCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                event == gamenet::net::ConnectorEvent::ConnectFailed);
            ++terminalFailures;
            reconnectAccepted =
                client.tryConnect() == gamenet::net::PostResult::Accepted;
        });

    gamenet::net::detail::TcpConnectionConstructionHarness::
        failNextBeforeSocketClaim();
    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);

    const auto progress = loop.runEvery(1ms, [&] {
        if (terminalFailures != 1 ||
            !reconnectAccepted ||
            connectedCallbacks != 1 ||
            disconnectedCallbacks != 1 ||
            acceptedSockets.size() != 2) {
            return;
        }
        client.stop();
        acceptor.stop();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        5s,
        "TcpClient construction failure did not release reconnect request");
    loop.cancel(progress);
    for (const auto fd : acceptedSockets) {
        gamenet::net::sockets::close(fd);
    }

    GAMENET_TEST_ASSERT(
        gamenet::net::detail::TcpConnectionConstructionHarness::
            failureWasConsumed());
    GAMENET_TEST_ASSERT(
        !gamenet::net::detail::TcpConnectionConstructionHarness::
            failureObservedSocketOwner());
    GAMENET_TEST_ASSERT(terminalFailures == 1);
    GAMENET_TEST_ASSERT(reconnectAccepted);
    GAMENET_TEST_ASSERT(connectedCallbacks == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbacks == 1);
    GAMENET_TEST_ASSERT(client.connection() == nullptr);
}

void verifyNormalClientLifecycle() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-contract-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-contract-client");

    bool connected = false;
    bool disconnected = false;

    server.start();

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            connected = true;
            GAMENET_TEST_ASSERT(client.connection() == conn);
            conn->forceClose();
            return;
        }

        disconnected = true;
        loop.queueInLoop([&] {
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            client.stop();
            server.stop();
            loop.quit();
        });
    });

    client.connect();
    gamenet::test::runLoopWithTimeout(loop, 2s, "timed out waiting for tcp client lifecycle");

    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(disconnected);
}

}  // namespace

int main() {
    verifyConstructionFailureReleasesRequestBeforeNotification();
    verifyNormalClientLifecycle();
    return 0;
}
