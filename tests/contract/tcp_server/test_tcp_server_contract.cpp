#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include <chrono>

namespace {

gamenet::net::SocketFd connectTo(const gamenet::net::InetAddress& serverAddr) {
    gamenet::net::SocketFd fd = gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    const int rc = gamenet::net::sockets::connect(fd, serverAddr.getSockAddr(), serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isInProgress(error) || gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "server-contract");

    bool connected = false;
    bool disconnected = false;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            connected = true;
            GAMENET_TEST_ASSERT(server.connectionCount() == 1);
            conn->forceClose();
            return;
        }

        disconnected = true;
        loop.queueInLoop([&] {
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            server.stop();
            loop.quit();
        });
    });

    server.start();
    gamenet::net::SocketFd clientFd = connectTo(server.listenAddress());

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for tcp server lifecycle");
        loop.quit();
    });
    loop.loop();

    gamenet::net::sockets::close(clientFd);

    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(disconnected);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
