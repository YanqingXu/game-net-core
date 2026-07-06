#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <string>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "echo");
    std::unique_ptr<gamenet::net::TcpClient> client;

    bool serverSawConnection = false;
    bool clientSawConnection = false;
    bool echoed = false;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            serverSawConnection = true;
        }
    });
    server.setMessageCallback([](const gamenet::net::TcpConnectionPtr& conn, gamenet::net::Buffer* buffer) {
        conn->send(buffer->retrieveAllAsString());
    });
    server.start();

    client = std::make_unique<gamenet::net::TcpClient>(&loop, server.listenAddress(), "client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            clientSawConnection = true;
            conn->send("hello");
        }
    });
    client->setMessageCallback([&](const gamenet::net::TcpConnectionPtr& conn, gamenet::net::Buffer* buffer) {
        echoed = true;
        assert(buffer->retrieveAllAsString() == "hello");
        conn->shutdown();
        client->stop();
        server.stop();
        loop.quit();
    });

    client->connect();
    loop.runAfter(5s, [&] {
        assert(false && "timed out waiting for tcp echo");
        loop.quit();
    });
    loop.loop();

    assert(serverSawConnection);
    assert(clientSawConnection);
    assert(echoed);

    return 0;
}
