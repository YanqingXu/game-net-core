#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <chrono>
#include <memory>
#include <stdexcept>

namespace {

void quitWhenComplete(gamenet::net::EventLoop* loop, bool accepted, bool connected) {
    if (accepted && connected) {
        loop->quit();
    }
}

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);
    auto connector = std::make_shared<gamenet::net::Connector>(&loop, acceptor.listenAddress());

    bool accepted = false;
    bool connected = false;
    int connectorEvents = 0;

    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&, gamenet::net::ConnectorEvent) {
            ++connectorEvents;
            throw std::runtime_error("connector observer failure");
        });

    acceptor.setNewConnectionCallback([&](gamenet::net::SocketFd sockfd, const gamenet::net::InetAddress&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(sockfd));

        accepted = true;
        gamenet::net::sockets::close(sockfd);
        quitWhenComplete(&loop, accepted, connected);
    });

    connector->setNewConnectionCallback([&](gamenet::net::SocketFd sockfd) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(sockfd));

        connected = true;
        gamenet::net::sockets::close(sockfd);
        quitWhenComplete(&loop, accepted, connected);
    });

    acceptor.listen();
    connector->start();

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for connector success");
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(accepted);
    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(connectorEvents >= 2);
    GAMENET_TEST_ASSERT(connector->state() == gamenet::net::Connector::kConnected);

    return 0;
}
