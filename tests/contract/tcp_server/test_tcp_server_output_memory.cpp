#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpOutputMemoryBudget.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main() {
    bool rejectedInvalidOptions = false;
    try {
        gamenet::net::TcpServerOutputMemoryOptions invalid;
        invalid.loop.recoveryThresholdBytes =
            invalid.loop.hardLimitBytes;
        invalid.validate();
    } catch (const std::invalid_argument&) {
        rejectedInvalidOptions = true;
    }
    GAMENET_TEST_ASSERT(rejectedInvalidOptions);

    gamenet::net::EventLoop loop;
    auto globalBudget =
        std::make_shared<gamenet::net::TcpOutputMemoryBudget>(
            gamenet::net::TcpOutputMemoryBudgetOptions{
                .hardLimitBytes = 700,
                .recoveryThresholdBytes = 350,
            });
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "hierarchical-output-memory");
    server.setThreadNum(2);
    server.setConnectionBackpressureOptions(
        gamenet::net::TcpConnectionBackpressureOptions{
            .lowWaterMarkBytes = 300,
            .highWaterMarkBytes = 600,
            .hardLimitBytes = 1200,
        });
    server.setOutputMemoryOptions(
        gamenet::net::TcpServerOutputMemoryOptions{
            .loop = {
                .hardLimitBytes = 1000,
                .recoveryThresholdBytes = 500,
            },
            .server = {
                .hardLimitBytes = 900,
                .recoveryThresholdBytes = 450,
            },
            .global = globalBudget,
        });

    std::array<
        std::atomic<gamenet::net::TcpSendResult>,
        4>
        results;
    for (auto& result : results) {
        result.store(
            gamenet::net::TcpSendResult::Accepted,
            std::memory_order_relaxed);
    }
    std::atomic<int> connectedCallbacks{0};
    std::atomic<int> disconnectedCallbacks{0};

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (!connection->connected()) {
                disconnectedCallbacks.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }

            connectedCallbacks.fetch_add(
                1, std::memory_order_relaxed);
            results[0].store(
                connection->trySend(std::string(750, 'g')),
                std::memory_order_relaxed);
            results[1].store(
                connection->trySend(std::string(950, 's')),
                std::memory_order_relaxed);
            results[2].store(
                connection->trySend(std::string(1050, 'l')),
                std::memory_order_relaxed);
            results[3].store(
                connection->trySend(std::string(1250, 'c')),
                std::memory_order_relaxed);
            GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
            connection->forceClose();
        });

    server.start();
    bool rejectedStartedChange = false;
    try {
        server.setOutputMemoryOptions({});
    } catch (const std::logic_error&) {
        rejectedStartedChange = true;
    }
    GAMENET_TEST_ASSERT(rejectedStartedChange);

    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto socket =
            gamenet::test::connectTestClient(
                server.listenAddress());
        const auto deadline =
            std::chrono::steady_clock::now() + 3s;
        char byte{};
        bool closed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto received =
                gamenet::net::sockets::read(
                    socket, &byte, sizeof(byte));
            if (received == 0) {
                closed = true;
                break;
            }
            if (received < 0) {
                const int error =
                    gamenet::net::sockets::lastError();
                if (!gamenet::net::sockets::isWouldBlock(error) &&
                    !gamenet::net::sockets::isInterrupted(error)) {
                    closed = true;
                    break;
                }
            }
            std::this_thread::sleep_for(2ms);
        }
        gamenet::test::closeTestSocket(socket);
        clientSucceeded.store(closed, std::memory_order_release);
        server.stop();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        5s,
        "timed out waiting for hierarchical output-memory stop");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(connectedCallbacks.load() == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbacks.load() == 1);
    GAMENET_TEST_ASSERT(
        results[0].load() ==
        gamenet::net::TcpSendResult::GlobalOverloaded);
    GAMENET_TEST_ASSERT(
        results[1].load() ==
        gamenet::net::TcpSendResult::ServerOverloaded);
    GAMENET_TEST_ASSERT(
        results[2].load() ==
        gamenet::net::TcpSendResult::LoopOverloaded);
    GAMENET_TEST_ASSERT(
        results[3].load() ==
        gamenet::net::TcpSendResult::Overloaded);

    const auto stats = server.outputMemoryStats();
    GAMENET_TEST_ASSERT(stats.loops.size() == 2);
    std::size_t loopPending = 0;
    std::size_t loopPeak = 0;
    std::uint64_t loopRejections = 0;
    for (const auto& loopStats : stats.loops) {
        loopPending += loopStats.pendingBytes;
        loopPeak =
            (std::max)(loopPeak, loopStats.peakPendingBytes);
        loopRejections += loopStats.rejectedReservations;
    }
    GAMENET_TEST_ASSERT(loopPending == 0);
    GAMENET_TEST_ASSERT(loopPeak == 950);
    GAMENET_TEST_ASSERT(loopRejections == 1);
    GAMENET_TEST_ASSERT(stats.server.pendingBytes == 0);
    // The 950-byte request is rejected before entering the server scope;
    // its peak therefore remains the earlier 750-byte reservation that was
    // rolled back after the global scope rejected it.
    GAMENET_TEST_ASSERT(stats.server.peakPendingBytes == 750);
    GAMENET_TEST_ASSERT(stats.server.rejectedReservations == 1);
    GAMENET_TEST_ASSERT(stats.global.has_value());
    GAMENET_TEST_ASSERT(stats.global->pendingBytes == 0);
    GAMENET_TEST_ASSERT(stats.global->peakPendingBytes == 0);
    GAMENET_TEST_ASSERT(
        stats.global->rejectedReservations == 1);
    GAMENET_TEST_ASSERT(
        globalBudget->snapshot().pendingBytes == 0);

    return 0;
}
