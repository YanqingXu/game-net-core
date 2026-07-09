#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TcpClientStopHarness.h"
#include "support/TestAssert.h"
#include <chrono>
#include <string>

using namespace std::chrono_literals;

int main() {
    constexpr int iterationCount = 3;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(
            &loop,
            gamenet::net::InetAddress(0, true),
            "retry-stop-soak-server-" + std::to_string(iteration));
        gamenet::net::TcpClient client(&loop, server.listenAddress(), "retry-stop-soak-client");
        gamenet::test::TcpClientStopHarness harness(loop);

        client.enableRetry();
        client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            harness.observeConnection(conn);
        });

        // client-retry-stop-soak: repeated stop() calls after retry scheduling
        // must cancel stale retry timers before a later server start can connect.
        client.connect();

        loop.runAfter(250ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            harness.markStopIssued();
            client.stop();
            server.start();
            harness.markServerStartedAfterStop();
        });

        loop.runAfter(900ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.stop();
            harness.assertStopped(client);
            loop.quit();
        });

        loop.loop();

        harness.assertStopped(client);
    }

    return 0;
}
