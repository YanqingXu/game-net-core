#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <chrono>
#include <string>

using namespace std::chrono_literals;

int main() {
    constexpr int iterationCount = 5;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(
            &loop,
            gamenet::net::InetAddress(0, true),
            "cross-thread-stop-pending-connect-server-" + std::to_string(iteration));
        gamenet::net::TcpClient client(
            &loop,
            server.listenAddress(),
            "cross-thread-stop-pending-connect-client-" + std::to_string(iteration));

        bool stopIssued = false;
        bool serverStartedAfterStop = false;
        bool connectedAfterStop = false;

        client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                connectedAfterStop = stopIssued;
                conn->forceClose();
            }
        });

        // client-cross-thread-stop-pending-connect: non-owner stop() must
        // marshal cancellation of an in-flight ConnectEx to the owner loop
        // before a later server start can publish a stale connection.
        client.connect();
        gamenet::test::runFromNonOwnerThread([&] {
            GAMENET_TEST_ASSERT(!loop.isInLoopThread());
            stopIssued = true;
            client.stop();
        });

        loop.runAfter(25ms + std::chrono::milliseconds(iteration * 5), [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.start();
            serverStartedAfterStop = true;
        });

        loop.runAfter(650ms, [&] {
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
