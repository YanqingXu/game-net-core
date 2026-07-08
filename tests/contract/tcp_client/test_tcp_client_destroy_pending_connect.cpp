#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include <chrono>
#include <memory>
#include <string>

using namespace std::chrono_literals;

int main() {
    constexpr int iterationCount = 5;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(
            &loop,
            gamenet::net::InetAddress(0, true),
            "destroy-pending-connect-server-" + std::to_string(iteration));
        std::unique_ptr<gamenet::net::TcpClient> client = std::make_unique<gamenet::net::TcpClient>(
            &loop,
            server.listenAddress(),
            "destroy-pending-connect-client-" + std::to_string(iteration));

        bool clientDestroyed = false;
        bool serverStartedAfterDestroy = false;
        bool connectedAfterDestroy = false;

        client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                connectedAfterDestroy = clientDestroyed;
                conn->forceClose();
            }
        });

        // client-destroy-pending-connect: owner-loop destruction during an
        // in-flight ConnectEx must clear stale callbacks and cancel/drain the
        // Connector before a later server start can publish a TcpConnection.
        client->connect();
        client.reset();
        clientDestroyed = true;

        loop.runAfter(25ms + std::chrono::milliseconds(iteration * 5), [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.start();
            serverStartedAfterDestroy = true;
        });

        loop.runAfter(650ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.stop();
            loop.quit();
        });

        loop.loop();

        GAMENET_TEST_ASSERT(clientDestroyed);
        GAMENET_TEST_ASSERT(serverStartedAfterDestroy);
        GAMENET_TEST_ASSERT(!connectedAfterDestroy);
        GAMENET_TEST_ASSERT(!client);
    }

    return 0;
}
