#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

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

        bool stopIssued = false;
        bool serverStartedAfterStop = false;
        bool connectedAfterStop = false;

        client.enableRetry();
        client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                connectedAfterStop = stopIssued;
                conn->forceClose();
            }
        });

        // client-retry-stop-soak: repeated stop() calls after retry scheduling
        // must cancel stale retry timers before a later server start can connect.
        client.connect();

        loop.runAfter(250ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            stopIssued = true;
            client.stop();
            server.start();
            serverStartedAfterStop = true;
        });

        loop.runAfter(900ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.stop();
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            loop.quit();
        });

        loop.loop();

        GAMENET_TEST_ASSERT(stopIssued);
        GAMENET_TEST_ASSERT(serverStartedAfterStop);
        GAMENET_TEST_ASSERT(!connectedAfterStop);
        GAMENET_TEST_ASSERT(client.connection() == nullptr);
    }

    return 0;
}
