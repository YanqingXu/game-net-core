#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/core/net/TimerId.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

void testImmediateStopDuringActiveWrite() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "server-stop-active-write-contract");

    const std::string payload(8 * 1024 * 1024, 'w');
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            ++connectedCallbackCount;
            GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
            GAMENET_TEST_ASSERT(server.connectionCount() == 1);

            conn->send(payload);
            server.stop();
            return;
        }

        ++disconnectedCallbackCount;
        GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
        loop.runAfter(std::chrono::milliseconds(50), [&] { loop.quit(); });
    });

    server.start();
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(2),
        "timed out waiting for server.stop() active-write teardown");

    gamenet::test::closeTestSocket(clientFd);

    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);

}

struct PublishedStopFutures {
    gamenet::net::TcpServerStopFuture first;
    gamenet::net::TcpServerStopFuture repeated;
};

void testGracefulStopDrainsOutputAndPublishesCompletion() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-graceful-stop-drain-contract");
    server.setThreadNum(1);

    const std::string payload(512 * 1024, 'd');
    std::promise<PublishedStopFutures> publishedPromise;
    auto publishedFuture = publishedPromise.get_future();
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopFuture repeatedFuture;
    gamenet::net::TcpServerStopResult result;
    std::atomic<int> connectedCallbackCount{0};
    std::atomic<int> disconnectedCallbackCount{0};
    std::size_t clientReadBytes = 0;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(conn->getLoop()->isInLoopThread());
        if (conn->connected()) {
            ++connectedCallbackCount;
            conn->send(payload);
            auto first = server.stopGracefully(gamenet::net::TcpServerStopOptions{
                .drainTimeout = 1000ms,
            });
            auto repeated = server.stopGracefully(gamenet::net::TcpServerStopOptions{
                .drainTimeout = 1ms,
            });
            publishedPromise.set_value(PublishedStopFutures{
                .first = std::move(first),
                .repeated = std::move(repeated),
            });
            return;
        }
        ++disconnectedCallbackCount;
    });

    server.start();
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());
    gamenet::net::TimerId drainTimer;
    gamenet::net::TimerId completionTimer;

    drainTimer = loop.runEvery(1ms, [&] {
        char buffer[16384];
        while (gamenet::net::sockets::isValid(clientFd)) {
            const ssize_t n = gamenet::net::sockets::read(clientFd, buffer, sizeof(buffer));
            if (n > 0) {
                clientReadBytes += static_cast<std::size_t>(n);
                GAMENET_TEST_ASSERT(clientReadBytes <= payload.size());
                continue;
            }
            if (n == 0) {
                gamenet::test::closeTestSocket(clientFd);
                clientFd = gamenet::net::kInvalidSocket;
                return;
            }
            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) ||
                gamenet::net::sockets::isInterrupted(error)) {
                return;
            }
            GAMENET_TEST_FAIL("unexpected client read error during graceful drain");
        }
    });

    completionTimer = loop.runEvery(1ms, [&] {
        if (!stopFuture.valid() && publishedFuture.wait_for(0ms) == std::future_status::ready) {
            auto published = publishedFuture.get();
            stopFuture = std::move(published.first);
            repeatedFuture = std::move(published.repeated);
        }
        if (!stopFuture.valid() || stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        result = stopFuture.get();
        const auto repeatedResult = repeatedFuture.get();
        GAMENET_TEST_ASSERT(repeatedResult.outcome == result.outcome);
        GAMENET_TEST_ASSERT(
            repeatedResult.initialConnectionCount == result.initialConnectionCount);
        loop.cancel(drainTimer);
        loop.cancel(completionTimer);
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "timed out waiting for graceful output drain completion");

    if (gamenet::net::sockets::isValid(clientFd)) {
        gamenet::test::closeTestSocket(clientFd);
    }
    GAMENET_TEST_ASSERT(result.outcome == gamenet::net::TcpServerStopOutcome::Drained);
    GAMENET_TEST_ASSERT(result.initialConnectionCount == 1);
    GAMENET_TEST_ASSERT(result.forcedConnectionCount == 0);
    GAMENET_TEST_ASSERT(clientReadBytes == payload.size());
    GAMENET_TEST_ASSERT(connectedCallbackCount.load() == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
}

void testGracefulStopTimeoutForcesRemainingConnection() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-graceful-stop-timeout-contract");
    server.setThreadNum(1);

    bool rejectedNegativeTimeout = false;
    try {
        (void)server.stopGracefully(gamenet::net::TcpServerStopOptions{
            .drainTimeout = -1ms,
        });
    } catch (const std::invalid_argument&) {
        rejectedNegativeTimeout = true;
    }
    GAMENET_TEST_ASSERT(rejectedNegativeTimeout);

    std::promise<gamenet::net::TcpServerStopFuture> publishedPromise;
    auto publishedFuture = publishedPromise.get_future();
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult result;
    std::atomic<int> disconnectedCallbackCount{0};

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            publishedPromise.set_value(server.stopGracefully(gamenet::net::TcpServerStopOptions{
                .drainTimeout = 50ms,
            }));
            return;
        }
        ++disconnectedCallbackCount;
    });

    server.start();
    const gamenet::net::SocketFd clientFd =
        gamenet::test::connectTestClient(server.listenAddress());
    gamenet::net::TimerId completionTimer;
    completionTimer = loop.runEvery(1ms, [&] {
        if (!stopFuture.valid() && publishedFuture.wait_for(0ms) == std::future_status::ready) {
            stopFuture = publishedFuture.get();
        }
        if (!stopFuture.valid() || stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        result = stopFuture.get();
        loop.cancel(completionTimer);
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        2s,
        "timed out waiting for graceful-stop forced fallback");

    gamenet::test::closeTestSocket(clientFd);
    GAMENET_TEST_ASSERT(
        result.outcome == gamenet::net::TcpServerStopOutcome::ForcedAfterTimeout);
    GAMENET_TEST_ASSERT(result.initialConnectionCount == 1);
    GAMENET_TEST_ASSERT(result.forcedConnectionCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount.load() == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
}

int main() {
    testImmediateStopDuringActiveWrite();
    testGracefulStopDrainsOutputAndPublishesCompletion();
    testGracefulStopTimeoutForcesRemainingConnection();
    return 0;
}
