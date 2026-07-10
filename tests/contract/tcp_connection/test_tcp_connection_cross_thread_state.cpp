#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "cross-thread-state#1");

    std::atomic<bool> observerReady{false};
    std::atomic<bool> sawConnected{false};
    std::atomic<bool> sawDisconnected{false};

    connection->setCloseCallback([](const gamenet::net::TcpConnectionPtr& conn) {
        conn->connectDestroyed();
    });

    std::thread observer([connection, &loop, &observerReady, &sawConnected, &sawDisconnected] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        GAMENET_TEST_ASSERT(!connection->connected());
        GAMENET_TEST_ASSERT(!connection->disconnected());
        observerReady.store(true, std::memory_order_release);

        while (!connection->connected() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(connection->connected());
        sawConnected.store(true, std::memory_order_relaxed);

        connection->forceClose();

        while (!connection->disconnected() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(connection->disconnected());
        sawDisconnected.store(true, std::memory_order_relaxed);
        loop.quit();
    });

    while (!observerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    loop.runAfter(0ms, [connection] { connection->connectEstablished(); });
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("cross-thread state observation timed out"); });
    loop.loop();
    observer.join();

    GAMENET_TEST_ASSERT(sawConnected.load(std::memory_order_relaxed));
    GAMENET_TEST_ASSERT(sawDisconnected.load(std::memory_order_relaxed));
    return 0;
}
