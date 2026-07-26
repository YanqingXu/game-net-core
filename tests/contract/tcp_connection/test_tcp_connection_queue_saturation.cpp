#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

gamenet::net::EventLoopOptions saturatedLoopOptions() {
    return gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    };
}

void drainPeer(
    gamenet::net::SocketFd peerFd,
    std::stop_token stopToken,
    std::atomic<std::size_t>& receivedBytes) {
    char buffer[64 * 1024];
    while (!stopToken.stop_requested()) {
        const auto n = gamenet::net::sockets::read(peerFd, buffer, sizeof(buffer));
        if (n > 0) {
            receivedBytes.fetch_add(
                static_cast<std::size_t>(n),
                std::memory_order_relaxed);
            continue;
        }
        if (n == 0) {
            return;
        }

        const int error = gamenet::net::sockets::lastError();
        if (gamenet::net::sockets::isWouldBlock(error) ||
            gamenet::net::sockets::isInterrupted(error)) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        return;
    }
}

void verifyHighWaterDropPreservesWriteProgress() {
    gamenet::net::EventLoop loop(saturatedLoopOptions());
    gamenet::test::ConnectedSocketPair pair(
        gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "queue-saturation-high-water");

    constexpr std::size_t lowWaterMark = 16 * 1024;
    constexpr std::size_t highWaterMark = 64 * 1024;
    constexpr std::size_t hardLimit = 4 * 1024 * 1024;
    const std::string payload(2 * 1024 * 1024, 'h');

    int highWaterCallbacks = 0;
    int closeCallbacks = 0;
    int saturatedQueueTasks = 0;
    bool observedPause = false;
    bool observedResume = false;
    bool closeRequested = false;
    std::atomic<std::size_t> receivedBytes{0};

    connection->setBackpressureOptions(
        gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = lowWaterMark,
            .highWaterMarkBytes = highWaterMark,
            .hardLimitBytes = hardLimit,
        });
    connection->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr&, std::size_t) {
            ++highWaterCallbacks;
        },
        highWaterMark);
    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        ++closeCallbacks;
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();

    loop.runEvery(1ms, [&] {
        if (connection->pendingOutputBytes() != 0 || closeRequested) {
            return;
        }
        observedResume = !connection->readingPausedByBackpressure();
        closeRequested = true;
        connection->forceClose();
    });
    loop.runAfter(5s, [&] {
        GAMENET_TEST_FAIL(
            "timed out waiting for saturated high-water write progress");
    });

    loop.queueInLoop([&] { ++saturatedQueueTasks; });
    loop.queueInLoop([&] { ++saturatedQueueTasks; });
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    std::jthread peerReader([&](std::stop_token stopToken) {
        drainPeer(pair.peerFd, stopToken, receivedBytes);
    });

    GAMENET_TEST_ASSERT(
        connection->trySend(payload) == gamenet::net::TcpSendResult::Accepted);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() > 0);
    observedPause = connection->readingPausedByBackpressure();
    GAMENET_TEST_ASSERT(observedPause);
    GAMENET_TEST_ASSERT(connection->droppedNotificationCount() == 1);
    GAMENET_TEST_ASSERT(highWaterCallbacks == 0);
    std::uint64_t crossThreadDroppedNotifications = 0;
    std::thread observer([&] {
        crossThreadDroppedNotifications =
            connection->droppedNotificationCount();
    });
    observer.join();
    GAMENET_TEST_ASSERT(crossThreadDroppedNotifications == 1);

    loop.loop();

    peerReader.request_stop();
    peerReader.join();

    GAMENET_TEST_ASSERT(saturatedQueueTasks == 2);
    GAMENET_TEST_ASSERT(observedPause);
    GAMENET_TEST_ASSERT(observedResume);
    GAMENET_TEST_ASSERT(highWaterCallbacks == 0);
    GAMENET_TEST_ASSERT(closeCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->droppedNotificationCount() == 1);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(connection->disconnected());
}

void verifyWriteCompleteDropPreservesDisconnectingHalfClose() {
    gamenet::net::EventLoop loop(saturatedLoopOptions());
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        pair,
        "queue-saturation-write-complete");

    const std::string payload = "disconnecting-half-close-after-notification-drop";
    int writeCompleteCallbacks = 0;
    int closeCallbacks = 0;
    bool keepQueueSaturated = true;
    bool closeRequested = false;
    std::atomic<bool> peerSawEof{false};
    std::string received;

    connection->setWriteCompleteCallback(
        [&](const gamenet::net::TcpConnectionPtr&) {
            ++writeCompleteCallbacks;
        });
    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        ++closeCallbacks;
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();

    std::function<void()> refillSaturatedQueue;
    refillSaturatedQueue = [&] {
        if (keepQueueSaturated) {
            loop.queueInLoop(refillSaturatedQueue);
        }
    };

    loop.runEvery(1ms, [&] {
        if (connection->droppedNotificationCount() == 1) {
            keepQueueSaturated = false;
        }
        if (!peerSawEof.load(std::memory_order_acquire) ||
            connection->droppedNotificationCount() != 1 ||
            closeRequested) {
            return;
        }
        closeRequested = true;
        connection->forceClose();
    });
    loop.runAfter(5s, [&] {
        GAMENET_TEST_FAIL(
            "timed out waiting for disconnecting half-close after notification drop");
    });

    loop.queueInLoop(refillSaturatedQueue);
    loop.queueInLoop(refillSaturatedQueue);
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    std::jthread peerReader([&](std::stop_token stopToken) {
        char buffer[256];
        while (!stopToken.stop_requested()) {
            const auto n =
                gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                received.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                peerSawEof.store(true, std::memory_order_release);
                return;
            }

            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) ||
                gamenet::net::sockets::isInterrupted(error)) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            return;
        }
    });

    GAMENET_TEST_ASSERT(
        connection->trySend(payload) == gamenet::net::TcpSendResult::Accepted);
    connection->shutdown();

    loop.loop();

    peerReader.join();

    GAMENET_TEST_ASSERT(peerSawEof.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(received == payload);
    GAMENET_TEST_ASSERT(writeCompleteCallbacks == 0);
    GAMENET_TEST_ASSERT(closeCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->droppedNotificationCount() == 1);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(connection->disconnected());
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1 || std::string(argv[1]) == "high-water") {
        verifyHighWaterDropPreservesWriteProgress();
    }
    if (argc == 1 || std::string(argv[1]) == "write-complete") {
        verifyWriteCompleteDropPreservesDisconnectingHalfClose();
    }
    return 0;
}
