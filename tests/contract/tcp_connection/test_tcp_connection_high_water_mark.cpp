#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "high-water-mark-fires-on-owner-loop");

    constexpr std::size_t highWaterMark = 1024;
    constexpr std::size_t policyLowWaterMark = 16 * 1024;
    constexpr std::size_t policyHighWaterMark = 64 * 1024;
    constexpr std::size_t policyHardLimit = 5 * 1024 * 1024;
    int highWaterCallbackCount = 0;
    int messageCallbackCount = 0;
    int closeCallbackCount = 0;
    std::size_t highWaterBytes = 0;
    bool sendReturned = false;
    bool highWaterBeforeSendReturned = false;
    bool observedReadPause = false;
    bool observedReadResume = false;
    std::atomic<bool> startDraining{false};
    std::atomic<std::size_t> drainedBytes{0};

    bool rejectedInvalidThresholds = false;
    try {
        connection->setBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = policyHighWaterMark,
            .highWaterMarkBytes = policyHighWaterMark,
            .hardLimitBytes = policyHardLimit,
        });
    } catch (const std::invalid_argument&) {
        rejectedInvalidThresholds = true;
    }
    GAMENET_TEST_ASSERT(rejectedInvalidThresholds);

    bool rejectedZeroInputLimit = false;
    try {
        connection->setBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = policyLowWaterMark,
            .highWaterMarkBytes = policyHighWaterMark,
            .hardLimitBytes = policyHardLimit,
            .maxInputBufferBytes = 0,
        });
    } catch (const std::invalid_argument&) {
        rejectedZeroInputLimit = true;
    }
    GAMENET_TEST_ASSERT(rejectedZeroInputLimit);

    connection->setBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
        .lowWaterMarkBytes = policyLowWaterMark,
        .highWaterMarkBytes = policyHighWaterMark,
        .hardLimitBytes = policyHardLimit,
    });

    connection->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn, gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(input->retrieveAllAsString() == "ping");
            observedReadResume = !conn->readingPausedByBackpressure();
            ++messageCallbackCount;
            conn->forceClose();
        });

    connection->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn, std::size_t bufferedBytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++highWaterCallbackCount;
            highWaterBytes = bufferedBytes;
            if (!sendReturned) {
                highWaterBeforeSendReturned = true;
            }
            observedReadPause = conn->readingPausedByBackpressure();
            const std::string inbound = "ping";
            const auto inboundWritten =
                gamenet::net::sockets::write(pair.peerFd, inbound.data(), inbound.size());
            GAMENET_TEST_ASSERT(
                inboundWritten == static_cast<decltype(inboundWritten)>(inbound.size()));
            loop.runAfter(std::chrono::milliseconds(25), [&] {
                GAMENET_TEST_ASSERT(messageCallbackCount == 0);
                startDraining.store(true, std::memory_order_release);
            });
        },
        highWaterMark);

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    const std::string oversizedPayload(policyHardLimit + 1, 'x');
    const std::string payload(4 * 1024 * 1024, 'h');
    std::jthread peerReader([&](std::stop_token stopToken) {
        char buffer[64 * 1024];
        while (!stopToken.stop_requested()) {
            if (!startDraining.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const auto n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                drainedBytes.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
                continue;
            }
            if (n == 0) {
                return;
            }
            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) ||
                gamenet::net::sockets::isInterrupted(error)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return;
        }
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        GAMENET_TEST_ASSERT(
            conn->trySend(oversizedPayload) == gamenet::net::TcpSendResult::Overloaded);
        GAMENET_TEST_ASSERT(conn->pendingOutputBytes() == 0);
        conn->send(payload);
        sendReturned = true;
        GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    });

    loop.runAfter(std::chrono::seconds(3), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for high-water callback");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(sendReturned);
    GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    GAMENET_TEST_ASSERT(highWaterCallbackCount == 1);
    GAMENET_TEST_ASSERT(highWaterBytes >= highWaterMark);
    GAMENET_TEST_ASSERT(observedReadPause);
    GAMENET_TEST_ASSERT(observedReadResume);
    GAMENET_TEST_ASSERT(messageCallbackCount == 1);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);

    peerReader.request_stop();

    return 0;
}
