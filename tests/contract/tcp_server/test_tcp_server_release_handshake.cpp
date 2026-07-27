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
#include <future>

using namespace std::chrono_literals;

int main() {
    constexpr int clientCount = 6;

    gamenet::net::EventLoop baseLoop;
    gamenet::net::TcpServer server(
        &baseLoop,
        gamenet::net::InetAddress(0, true),
        "server-release-handshake-contract");
    server.setThreadNum(2);

    std::atomic<int> connected{0};
    std::atomic<int> disconnected{0};
    std::atomic<bool> reenteredStop{false};
    std::promise<gamenet::net::TcpServerStopFuture> publishedPromise;
    auto publishedFuture = publishedPromise.get_future();
    gamenet::test::WorkerLoopTracker workerLoops;

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(
                connection->getLoop()->isInLoopThread());
            workerLoops.recordCurrentThread();
            if (connection->connected()) {
                if (connected.fetch_add(1) + 1 == clientCount) {
                    publishedPromise.set_value(
                        server.stopGracefully(
                            gamenet::net::TcpServerStopOptions{
                                .drainTimeout = 0ms,
                            }));
                }
                return;
            }

            GAMENET_TEST_ASSERT(connection->socketClosed());
            GAMENET_TEST_ASSERT(
                connection->closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            disconnected.fetch_add(1);
            if (!reenteredStop.exchange(true)) {
                // Re-entry while worker cleanup is active must coalesce with
                // the current generation rather than starting an early join.
                server.stop();
            }
        });

    server.start();
    auto clients =
        gamenet::test::connectTestClients(server.listenAddress(), clientCount);
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult result;

    const auto completionTimer = baseLoop.runEvery(1ms, [&] {
        if (!stopFuture.valid() &&
            publishedFuture.wait_for(0ms) == std::future_status::ready) {
            stopFuture = publishedFuture.get();
        }
        if (!stopFuture.valid()) {
            return;
        }
        if (stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        result = stopFuture.get();
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
        baseLoop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        baseLoop,
        4s,
        "TcpServer BaseReleased/worker-ack handshake did not converge");
    baseLoop.cancel(completionTimer);
    gamenet::test::closeTestSockets(clients);

    workerLoops.requireAtLeast(2);
    GAMENET_TEST_ASSERT(connected.load() == clientCount);
    GAMENET_TEST_ASSERT(disconnected.load() == clientCount);
    GAMENET_TEST_ASSERT(reenteredStop.load());
    GAMENET_TEST_ASSERT(
        result.outcome ==
        gamenet::net::TcpServerStopOutcome::ForcedByImmediateStop);
    GAMENET_TEST_ASSERT(
        result.initialConnectionCount ==
        static_cast<std::size_t>(clientCount));
    GAMENET_TEST_ASSERT(
        result.forcedConnectionCount ==
        static_cast<std::size_t>(clientCount));
    return 0;
}
