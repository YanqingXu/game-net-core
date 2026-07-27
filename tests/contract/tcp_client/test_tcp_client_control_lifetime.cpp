#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    });
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop,
        gamenet::net::InetAddress("127.0.0.1", 9),
        "tcp-client-control-lifetime");
    auto control = client->control();
    std::atomic<int> terminalFailures{0};
    client->setTerminalConnectFailureCallback(
        [&](const auto&, auto) { terminalFailures.fetch_add(1); });

    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 1);
    loop.queueInLoop([] {});
    loop.queueInLoop([] {});
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    gamenet::net::PostResult connectResult =
        gamenet::net::PostResult::OwnerUnavailable;
    gamenet::net::PostResult stopResult =
        gamenet::net::PostResult::OwnerUnavailable;
    std::thread requester([&] {
        connectResult = control.tryConnect();
        stopResult = control.tryStop();
    });
    requester.join();

    GAMENET_TEST_ASSERT(
        connectResult == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        stopResult == gamenet::net::PostResult::Accepted);

    loop.runAfter(20ms, [&] {
        GAMENET_TEST_ASSERT(client->connection() == nullptr);
        GAMENET_TEST_ASSERT(terminalFailures.load() == 0);
        client.reset();
        GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
        GAMENET_TEST_ASSERT(
            control.tryConnect() ==
            gamenet::net::PostResult::OwnerUnavailable);
        GAMENET_TEST_ASSERT(
            control.tryDisconnect() ==
            gamenet::net::PostResult::OwnerUnavailable);
        GAMENET_TEST_ASSERT(
            control.tryStop() ==
            gamenet::net::PostResult::OwnerUnavailable);
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        2s,
        "TcpClientControl did not coalesce or detach deterministically");
    return 0;
}
