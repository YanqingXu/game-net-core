#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "support/TestAssert.h"
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "client-contract-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "client-contract-client");

    bool connected = false;
    bool disconnected = false;

    server.start();

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            connected = true;
            GAMENET_TEST_ASSERT(client.connection() == conn);
            conn->forceClose();
            return;
        }

        disconnected = true;
        loop.queueInLoop([&] {
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            client.stop();
            server.stop();
            loop.quit();
        });
    });

    client.connect();
    loop.runAfter(2s, [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for tcp client lifecycle");
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(disconnected);

    return 0;
}
