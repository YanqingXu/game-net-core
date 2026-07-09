#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <chrono>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "server-stop-active-write-contract");

    const std::string payload(8 * 1024 * 1024, 'w');
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            ++connectedCallbackCount;
            GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
            GAMENET_TEST_ASSERT(server.connectionCount() == 1);

            conn->send(payload);
            server.stop();
            return;
        }

        ++disconnectedCallbackCount;
        GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
        loop.runAfter(std::chrono::milliseconds(50), [&] { loop.quit(); });
    });

    server.start();
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(2),
        "timed out waiting for server.stop() active-write teardown");

    gamenet::test::closeTestSocket(clientFd);

    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
