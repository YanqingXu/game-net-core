#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include <chrono>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "pending-connect-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "pending-connect-client");

    bool stopIssued = false;
    bool serverStartedAfterStop = false;
    bool connectedAfterStop = false;

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            connectedAfterStop = true;
            conn->forceClose();
        }
    });

    // client-stop-pending-connect: stop() must cancel an in-flight ConnectEx
    // before a later server start can accept a stale completion.
    client.connect();
    client.stop();
    stopIssued = true;

    loop.runAfter(50ms, [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        server.start();
        serverStartedAfterStop = true;
    });

    loop.runAfter(600ms, [&] {
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

    return 0;
}
