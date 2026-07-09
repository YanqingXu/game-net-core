#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TcpClientStopHarness.h"
#include "support/TestAssert.h"
#include <chrono>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "retry-stop-server");
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "retry-stop-client");
    gamenet::test::TcpClientStopHarness harness(loop);

    client.enableRetry();
    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        harness.observeConnection(conn);
    });

    // client-retry-stop-race: stop() must cancel any in-flight connect or
    // pending retry before a later server start can accept a stale retry.
    client.connect();
    loop.runAfter(250ms, [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        harness.markStopIssued();
        client.stop();
        server.start();
        harness.markServerStartedAfterStop();
    });

    loop.runAfter(1200ms, [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        server.stop();
        harness.assertStopped(client);
        loop.quit();
    });

    loop.loop();

    harness.assertStopped(client);

    return 0;
}
