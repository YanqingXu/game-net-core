#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>

int main() {
    gamenet::net::EventLoop baseLoop;
    gamenet::net::TcpServer server(
        &baseLoop,
        gamenet::net::InetAddress(0, true),
        "server-repeated-stop-idempotent");
    server.setThreadNum(1);

    std::atomic<int> connectedCallbackCount{0};
    std::atomic<int> disconnectedCallbackCount{0};
    std::atomic<bool> repeatedStopIssued{false};

    auto issueRepeatedStop = [&] {
        GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
        GAMENET_TEST_ASSERT(server.connectionCount() == 1);

        server.stop();
        server.stop();
        baseLoop.queueInLoop([&] { server.stop(); });
    };

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(conn->getLoop()->isInLoopThread());

        if (conn->connected()) {
            const int connected = connectedCallbackCount.fetch_add(1) + 1;
            GAMENET_TEST_ASSERT(connected == 1);

            baseLoop.runInLoop([&] {
                repeatedStopIssued = true;
                issueRepeatedStop();
            });
            return;
        }

        const int disconnected = disconnectedCallbackCount.fetch_add(1) + 1;
        GAMENET_TEST_ASSERT(disconnected == 1);
        baseLoop.runInLoop([&] {
            GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            baseLoop.quit();
        });
    });

    server.start();
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());

    gamenet::test::runLoopWithTimeout(
        baseLoop,
        std::chrono::seconds(2),
        "timed out waiting for repeated TcpServer::stop() teardown");

    gamenet::test::closeTestSocket(clientFd);

    GAMENET_TEST_ASSERT(repeatedStopIssued.load());
    GAMENET_TEST_ASSERT(connectedCallbackCount.load() == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    return 0;
}
