#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/LoopTest.h"
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
            "stop-active-connection-server-" + std::to_string(iteration));
        gamenet::net::TcpClient client(
            &loop,
            server.listenAddress(),
            "stop-active-connection-client-" + std::to_string(iteration));

        client.enableRetry();

        std::atomic<bool> stopIssued{false};
        std::atomic<bool> ownerStopIssued{false};
        std::atomic<bool> nonOwnerStopIssued{false};
        bool peerCloseIssued = false;
        int clientConnectedCount = 0;
        int clientDisconnectedCount = 0;
        int serverConnectedCount = 0;
        int serverDisconnectedCount = 0;
        gamenet::net::TcpConnectionPtr serverConnection;
        gamenet::test::NonOwnerThreadHandoff stopper;

        auto closePeerAfterStop = [&] {
            if (peerCloseIssued) {
                return;
            }
            peerCloseIssued = true;
            loop.runAfter(80ms + std::chrono::milliseconds(iteration), [&] {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(stopIssued.load());
                GAMENET_TEST_ASSERT(serverConnection);
                serverConnection->forceClose();
            });
        };

        server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                ++serverConnectedCount;
                GAMENET_TEST_ASSERT(serverConnectedCount == 1);
                serverConnection = conn;
                return;
            }

            ++serverDisconnectedCount;
            GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
            serverConnection.reset();
        });

        client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                ++clientConnectedCount;
                GAMENET_TEST_ASSERT(clientConnectedCount == 1);
                GAMENET_TEST_ASSERT(client.connection() == conn);

                // client-stop-active-connection-mixed-timing-soak: stop()
                // on an active retry-enabled client must clear future connect
                // intent from owner and non-owner timing paths, so peer close
                // cannot resurrect the client through retry.
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

                closePeerAfterStop();
                return;
            }

            ++clientDisconnectedCount;
            GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        });

        server.start();
        client.connect();

        loop.runAfter(900ms, [&] {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(stopIssued.load());
            GAMENET_TEST_ASSERT(ownerStopIssued.load() || nonOwnerStopIssued.load());
            GAMENET_TEST_ASSERT(peerCloseIssued);
            GAMENET_TEST_ASSERT(clientConnectedCount == 1);
            GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
            GAMENET_TEST_ASSERT(serverConnectedCount == 1);
            GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
            GAMENET_TEST_ASSERT(client.connection() == nullptr);
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            server.stop();
            loop.quit();
        });

        gamenet::test::runLoopWithTimeout(
            loop,
            2s,
            "timed out waiting for active TcpClient stop soak teardown");

        stopper.join();

        GAMENET_TEST_ASSERT(stopIssued.load());
        GAMENET_TEST_ASSERT(ownerStopIssued.load() || nonOwnerStopIssued.load());
        GAMENET_TEST_ASSERT(clientConnectedCount == 1);
        GAMENET_TEST_ASSERT(clientDisconnectedCount == 1);
        GAMENET_TEST_ASSERT(serverConnectedCount == 1);
        GAMENET_TEST_ASSERT(serverDisconnectedCount == 1);
        GAMENET_TEST_ASSERT(client.connection() == nullptr);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    }

    return 0;
}
