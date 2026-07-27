#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <chrono>
#include <optional>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "close-reason-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "close-reason-client");

    std::optional<gamenet::net::TcpConnectionCloseInfo> serverClose;
    std::optional<gamenet::net::TcpConnectionCloseInfo> clientClose;
    server.setCloseInfoCallback(
        [&](const auto&, gamenet::net::TcpConnectionCloseInfo info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            serverClose = info;
        });
    client.setCloseInfoCallback(
        [&](const auto&, gamenet::net::TcpConnectionCloseInfo info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            clientClose = info;
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                GAMENET_TEST_ASSERT(
                    client.tryDisconnect() ==
                    gamenet::net::PostResult::Accepted);
            }
        });

    server.start();
    client.connect();

    const auto completionTimer = loop.runEvery(1ms, [&] {
        if (!serverClose || !clientClose) {
            return;
        }
        GAMENET_TEST_ASSERT(
            serverClose->reason ==
            gamenet::net::TcpConnectionCloseReason::PeerEof);
        GAMENET_TEST_ASSERT(
            clientClose->reason ==
            gamenet::net::TcpConnectionCloseReason::GracefulShutdown);
        client.stop();
        server.stop();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "structured close reason did not reach TcpServer and TcpClient");
    loop.cancel(completionTimer);
    return 0;
}
