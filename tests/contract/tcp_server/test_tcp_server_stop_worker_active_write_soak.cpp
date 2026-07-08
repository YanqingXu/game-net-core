#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

gamenet::net::SocketFd connectTo(const gamenet::net::InetAddress& serverAddr) {
    gamenet::net::SocketFd fd = gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    const int rc = gamenet::net::sockets::connect(fd, serverAddr.getSockAddr(), serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isInProgress(error) || gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

}  // namespace

int main() {
    constexpr int iterationCount = 3;
    constexpr int clientCount = 2;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::TcpServer server(
            &baseLoop,
            gamenet::net::InetAddress(0, true),
            "server-stop-worker-active-write-soak-" + std::to_string(iteration));
        server.setThreadNum(2);

        const std::string payload(8 * 1024 * 1024, 'w');
        std::atomic<int> connectedCallbackCount{0};
        std::atomic<int> disconnectedCallbackCount{0};
        std::atomic<bool> stopQueued{false};
        std::mutex workerLoopIdsMutex;
        std::set<std::thread::id> workerLoopIds;

        server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(conn->getLoop()->isInLoopThread());

            if (conn->connected()) {
                {
                    std::lock_guard lock(workerLoopIdsMutex);
                    workerLoopIds.insert(std::this_thread::get_id());
                }

                conn->send(payload);
                const int connected = connectedCallbackCount.fetch_add(1) + 1;
                if (connected == clientCount && !stopQueued.exchange(true)) {
                    baseLoop.runInLoop([&] {
                        GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
                        GAMENET_TEST_ASSERT(server.connectionCount() == clientCount);

                        // server-stop-worker-active-write-soak: base-loop
                        // stop() must force-close worker-owned connections
                        // while their owner loops may still have pending writes.
                        server.stop();
                    });
                }
                return;
            }

            const int disconnected = disconnectedCallbackCount.fetch_add(1) + 1;
            GAMENET_TEST_ASSERT(disconnected <= clientCount);
            if (disconnected == clientCount) {
                baseLoop.runInLoop([&] {
                    GAMENET_TEST_ASSERT(baseLoop.isInLoopThread());
                    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
                    baseLoop.runAfter(std::chrono::milliseconds(50), [&] { baseLoop.quit(); });
                });
            }
        });

        server.start();

        std::vector<gamenet::net::SocketFd> clientFds;
        clientFds.reserve(clientCount);
        for (int i = 0; i < clientCount; ++i) {
            clientFds.push_back(connectTo(server.listenAddress()));
        }

        baseLoop.runAfter(std::chrono::seconds(3), [&] {
            GAMENET_TEST_ASSERT(false && "timed out waiting for worker-owned active-write server.stop() teardown");
            baseLoop.quit();
        });
        baseLoop.loop();

        for (const auto fd : clientFds) {
            gamenet::net::sockets::close(fd);
        }

        {
            std::lock_guard lock(workerLoopIdsMutex);
            GAMENET_TEST_ASSERT(workerLoopIds.size() >= 2);
        }
        GAMENET_TEST_ASSERT(stopQueued.load());
        GAMENET_TEST_ASSERT(connectedCallbackCount.load() == clientCount);
        GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == clientCount);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    }

    return 0;
}
