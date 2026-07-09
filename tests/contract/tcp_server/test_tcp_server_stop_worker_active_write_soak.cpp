#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TcpServerHarness.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <string>

int main() {
    constexpr int iterationCount = 3;
    constexpr int clientCount = 2;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::TcpServer server(
            &baseLoop,
            gamenet::net::InetAddress(0, true),
            "server-stop-worker-active-write-soak-" + std::to_string(iteration));
        server.setThreadNum(2);

        const std::string payload(8 * 1024 * 1024, 'w');
        std::atomic<int> connectedCallbackCount{0};
        std::atomic<int> disconnectedCallbackCount{0};
        std::atomic<bool> stopQueued{false};
        gamenet::test::WorkerLoopTracker workerLoopTracker;

        server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(conn->getLoop()->isInLoopThread());

            if (conn->connected()) {
                workerLoopTracker.recordCurrentThread();

                conn->send(payload);
                const int connected = connectedCallbackCount.fetch_add(1) + 1;
                if (connected == clientCount && !stopQueued.exchange(true)) {
                    baseLoop.runInLoop([&] {
                        GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
                        GAMENET_TEST_ASSERT(server.connectionCount() == clientCount);

                        // server-stop-worker-active-write-soak: base-loop
                        // stop() must force-close worker-owned connections
                        // while their owner loops may still have pending writes.
                        server.stop();
                    });
                }
                return;
            }

            const int disconnected = disconnectedCallbackCount.fetch_add(1) + 1;
            GAMENET_TEST_ASSERT(disconnected <= clientCount);
            if (disconnected == clientCount) {
                baseLoop.runInLoop([&] {
                    GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
                    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
                    baseLoop.runAfter(std::chrono::milliseconds(50), [&] { baseLoop.quit(); });
                });
            }
        });

        server.start();
        auto clientFds = gamenet::test::connectTestClients(server.listenAddress(), clientCount);

        gamenet::test::runLoopWithTimeout(
            baseLoop,
            std::chrono::seconds(3),
            "timed out waiting for worker-owned active-write server.stop() teardown");

        gamenet::test::closeTestSockets(clientFds);

        workerLoopTracker.requireAtLeast(2);
        GAMENET_TEST_ASSERT(stopQueued.load());
        GAMENET_TEST_ASSERT(connectedCallbackCount.load() == clientCount);
        GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == clientCount);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    }

    return 0;
}
