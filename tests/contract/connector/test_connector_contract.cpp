#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cassert>
#include <chrono>
#include <memory>

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

    acceptor.setNewConnectionCallback([&](gamenet::net::SocketFd sockfd, const gamenet::net::InetAddress&) {
        assert(loop.isInLoopThread());
        assert(gamenet::net::sockets::isValid(sockfd));

        accepted = true;
        gamenet::net::sockets::close(sockfd);
        quitWhenComplete(&loop, accepted, connected);
    });

    connector->setNewConnectionCallback([&](gamenet::net::SocketFd sockfd) {
        assert(loop.isInLoopThread());
        assert(gamenet::net::sockets::isValid(sockfd));

        connected = true;
        gamenet::net::sockets::close(sockfd);
        quitWhenComplete(&loop, accepted, connected);
    });

    acceptor.listen();
    connector->start();

    loop.runAfter(std::chrono::seconds(1), [&] {
        assert(false && "timed out waiting for connector success");
        loop.quit();
    });
    loop.loop();

    assert(accepted);
    assert(connected);
    assert(connector->state() == gamenet::net::Connector::kConnected);

    return 0;
}
