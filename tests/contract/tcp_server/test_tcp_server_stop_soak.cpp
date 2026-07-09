#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <string>

int main() {
    constexpr int iterationCount = 3;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::TcpServer server(
            &baseLoop,
            gamenet::net::InetAddress(0, true),
            "server-stop-soak-contract-" + std::to_string(iteration));
        server.setThreadNum(1);

        std::atomic<int> connectedCallbackCount{0};
        std::atomic<int> disconnectedCallbackCount{0};
        std::atomic<bool> stopIssued{false};

        server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(conn->getLoop()->isInLoopThread());

            if (conn->connected()) {
                ++connectedCallbackCount;
                GAMENET_TEST_ASSERT(connectedCallbackCount.load() == 1);

                // server-stop-soak-contract: stop() from a worker-owned
                // connection must let connection teardown finish before the
                // worker pool is joined.
                stopIssued = true;
                server.stop();
                return;
            }

            ++disconnectedCallbackCount;
            GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1);
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
            "timed out waiting for server.stop() worker-loop soak teardown");

        gamenet::test::closeTestSocket(clientFd);

        GAMENET_TEST_ASSERT(stopIssued.load());
        GAMENET_TEST_ASSERT(connectedCallbackCount.load() == 1);
        GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    }

    return 0;
}
