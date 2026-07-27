#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-saturation-shutdown-contract");
    server.setThreadNum(2);
    server.start();

    // Occupy both normal and reserved pending-functor capacity before the
    // cross-thread stop request. Stop admission must use the lifecycle lane.
    loop.queueInLoop([] {});
    loop.queueInLoop([] {});
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    const auto lifecycleSignalsBefore = loop.lifecycleSignalCount();
    std::promise<gamenet::net::TcpServerStopFuture> publishedPromise;
    auto publishedFuture = publishedPromise.get_future();
    std::thread requester([&] {
        publishedPromise.set_value(
            server.stopGracefully(gamenet::net::TcpServerStopOptions{
                .drainTimeout = 250ms,
            }));
    });
    requester.join();

    auto stopFuture = publishedFuture.get();
    gamenet::net::TcpServerStopResult result;
    const auto completionTimer = loop.runEvery(1ms, [&] {
        if (stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        result = stopFuture.get();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "aggregate TcpServer stop did not survive saturated base queues");
    loop.cancel(completionTimer);

    GAMENET_TEST_ASSERT(
        result.outcome == gamenet::net::TcpServerStopOutcome::Drained);
    GAMENET_TEST_ASSERT(result.initialConnectionCount == 0);
    GAMENET_TEST_ASSERT(result.forcedConnectionCount == 0);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

    const auto lifecycleSignals =
        loop.lifecycleSignalCount() - lifecycleSignalsBefore;
    // One committed base request plus quiet/ack notifications from two worker
    // aggregates. Connection count must not affect this bound.
    GAMENET_TEST_ASSERT(lifecycleSignals >= 1);
    GAMENET_TEST_ASSERT(lifecycleSignals <= 5);
    return 0;
}
