#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include <cassert>
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
        assert(loop.isInLoopThread());

        if (conn->connected()) {
            connected = true;
            assert(client.connection() == conn);
            conn->forceClose();
            return;
        }

        disconnected = true;
        loop.queueInLoop([&] {
            assert(client.connection() == nullptr);
            client.stop();
            server.stop();
            loop.quit();
        });
    });

    client.connect();
    loop.runAfter(2s, [&] {
        assert(false && "timed out waiting for tcp client lifecycle");
        loop.quit();
    });
    loop.loop();

    assert(connected);
    assert(disconnected);

    return 0;
}
