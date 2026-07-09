#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <atomic>
#include <chrono>
#include <string>

using namespace std::chrono_literals;

int main() {
    constexpr int iterationCount = 8;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(
            &loop,
            gamenet::net::InetAddress(0, true),
            "mixed-timing-pending-connect-server-" + std::to_string(iteration));
        gamenet::net::TcpClient client(
            &loop,
            server.listenAddress(),
            "mixed-timing-pending-connect-client-" + std::to_string(iteration));

        std::atomic<bool> stopIssued{false};
        std::atomic<bool> ownerStopIssued{false};
        std::atomic<bool> nonOwnerStopIssued{false};
        bool serverStartedAfterStop = false;
        bool connectedAfterStop = false;

        client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                connectedAfterStop = stopIssued.load();
                conn->forceClose();
            }
        });

        // client-stop-pending-connect-mixed-timing-soak: owner-thread and
        // non-owner stop() calls, both immediate and delayed while the loop is
        // running, must cancel an in-flight ConnectEx before a later server
        // start can publish a stale TcpConnection.
        client.connect();

        gamenet::test::NonOwnerThreadHandoff stopper;
        const int mode = iteration % 4;
        if (mode == 0) {
            stopIssued = true;
            ownerStopIssued = true;
            client.stop();
        } else if (mode == 1) {
            stopper = gamenet::test::startNonOwnerThread([&] {
                GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                stopIssued = true;
                nonOwnerStopIssued = true;
                client.stop();
            });
        } else if (mode == 2) {
            loop.runAfter(5ms + std::chrono::milliseconds(iteration), [&] {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                stopIssued = true;
                ownerStopIssued = true;
                client.stop();
            });
        } else {
            stopper = gamenet::test::startNonOwnerThreadAfter(5ms + std::chrono::milliseconds(iteration), [&] {
                GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                stopIssued = true;
                nonOwnerStopIssued = true;
                client.stop();
            });
        }

        loop.runAfter(55ms + std::chrono::milliseconds(iteration * 3), [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(stopIssued.load());
            server.start();
            serverStartedAfterStop = true;
        });

        loop.runAfter(700ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            server.stop();
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            loop.quit();
        });

        loop.loop();

        stopper.join();

        GAMENET_TEST_ASSERT(stopIssued.load());
        GAMENET_TEST_ASSERT(ownerStopIssued.load() || nonOwnerStopIssued.load());
        GAMENET_TEST_ASSERT(serverStartedAfterStop);
        GAMENET_TEST_ASSERT(!connectedAfterStop);
        GAMENET_TEST_ASSERT(client.connection() == nullptr);
    }

    return 0;
}
