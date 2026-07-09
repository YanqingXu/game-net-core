#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <chrono>

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
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "timed out waiting for tcp server lifecycle");

    gamenet::test::closeTestSocket(clientFd);

    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(disconnected);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
