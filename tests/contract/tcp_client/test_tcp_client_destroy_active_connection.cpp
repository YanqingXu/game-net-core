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
            "destroy-active-connection-server-" + std::to_string(iteration));
        std::unique_ptr<gamenet::net::TcpClient> client = std::make_unique<gamenet::net::TcpClient>(
            &loop,
            server.listenAddress(),
            "destroy-active-connection-client-" + std::to_string(iteration));

        int clientConnectedCount = 0;
        int clientDisconnectedCount = 0;
        int serverConnectedCount = 0;
        int serverDisconnectedCount = 0;
        bool clientDestroyed = false;
        bool serverDisconnectedAfterDestroy = false;

        server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                ++serverConnectedCount;
                return;
            }

            ++serverDisconnectedCount;
            serverDisconnectedAfterDestroy = clientDestroyed;
            loop.runAfter(50ms, [&] {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(server.connectionCount() == 0);
                server.stop();
                loop.quit();
            });
        });

        client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                ++clientConnectedCount;
                GAMENET_TEST_ASSERT(client);
                GAMENET_TEST_ASSERT(client->connection() == conn);

                // client-destroy-active-connection: owner-loop destruction
                // with an active TcpConnection must remove callbacks that
                // point back to TcpClient and let peer teardown converge.
                loop.runAfter(10ms, [&] {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread());
                    GAMENET_TEST_ASSERT(client);
                    clientDestroyed = true;
                    client.reset();
                });
                return;
            }

            ++clientDisconnectedCount;
        });

        server.start();
        client->connect();

        loop.runAfter(2s, [&] {
            GAMENET_TEST_ASSERT(false && "timed out waiting for active TcpClient destruction teardown");
            loop.quit();
        });

        loop.loop();

        GAMENET_TEST_ASSERT(clientConnectedCount == 1);
        GAMENET_TEST_ASSERT(serverConnectedCount == 1);
        GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
        GAMENET_TEST_ASSERT(serverDisconnectedAfterDestroy);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
        GAMENET_TEST_ASSERT(!client);
        GAMENET_TEST_ASSERT(clientDisconnectedCount <= 1);
    }

    return 0;
}
