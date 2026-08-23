// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "experimental/io_uring/IoUringTcpMultiOwnerServer.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

namespace uring = gamenet::experimental::io_uring;

class OwnedFd {
public:
    explicit OwnedFd(int value = -1) noexcept : value_(value) {}
    ~OwnedFd() { close(); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this == &other) return *this;
        close();
        value_ = std::exchange(other.value_, -1);
        return *this;
    }

    int get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ >= 0; }
    void close() noexcept {
        if (!valid()) return;
        ::close(value_);
        value_ = -1;
    }

private:
    int value_;
};

OwnedFd connectClient(const gamenet::net::InetAddress& address) {
    OwnedFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    GAMENET_TEST_ASSERT(client.valid());
    const auto& native = address.getSockAddrInet();
    while (::connect(
               client.get(),
               reinterpret_cast<const sockaddr*>(&native),
               sizeof(native)) != 0) {
        GAMENET_TEST_ASSERT(errno == EINTR);
    }
    const auto flags = ::fcntl(client.get(), F_GETFL, 0);
    GAMENET_TEST_ASSERT(flags >= 0);
    GAMENET_TEST_ASSERT(
        ::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK) == 0);
    return client;
}

void sendAll(int descriptor, std::string_view payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto count = ::send(
            descriptor,
            payload.data() + offset,
            payload.size() - offset,
            MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            GAMENET_TEST_FAIL("failed to send X12 payload");
        }
    }
}

bool drainAvailable(OwnedFd& client, std::string& output) {
    std::array<char, 64> buffer{};
    while (client.valid()) {
        const auto count = ::recv(
            client.get(), buffer.data(), buffer.size(), 0);
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            client.close();
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        if (errno == ECONNRESET || errno == ENOTCONN) {
            client.close();
            return true;
        }
        GAMENET_TEST_FAIL("unexpected X12 client receive failure");
    }
    return true;
}

uring::IoUringTcpMultiOwnerServerOptions serverOptions(
    gamenet::net::EventLoopSelectionPolicy policy) {
    return {
        .acceptHub = {
            .pump = {
                .engine = {
                    .entries = 32,
                    .maxOperations = 16,
                    .maxCompletionsPerWait = 16,
                    .maxBytesPerOperation = 16,
                    .maxOwnedBytes = 128,
                },
                .maxNoticesPerTurn = 2,
            },
            .maxConnections = 4,
            .maxTotalPendingSendBytes = 128,
            .maxReceiveBytes = 16,
            .maxSendBytesPerOperation = 16,
            .maxPendingSendBytesPerConnection = 32,
            .maxPendingSendSegmentsPerConnection = 4,
            .maxPendingAccepts = 2,
        },
        .workerHub = {
            .pump = {
                .engine = {
                    .entries = 32,
                    .maxOperations = 16,
                    .maxCompletionsPerWait = 16,
                    .maxBytesPerOperation = 16,
                    .maxOwnedBytes = 128,
                },
                .maxNoticesPerTurn = 2,
            },
            .maxConnections = 4,
            .maxTotalPendingSendBytes = 128,
            .maxReceiveBytes = 16,
            .maxSendBytesPerOperation = 16,
            .maxPendingSendBytesPerConnection = 32,
            .maxPendingSendSegmentsPerConnection = 4,
            .maxPendingAccepts = 1,
        },
        .connection = {
            .lowWaterMarkBytes = 4,
            .highWaterMarkBytes = 8,
            .hardLimitBytes = 32,
            .maxPendingCommands = 8,
            .maxCommandsPerTurn = 4,
        },
        .workerCount = 2,
        .maxPendingHandoffs = 4,
        .placementPolicy = policy,
        .reuseAddress = true,
        .reusePort = false,
    };
}

void assertZeroResidue(
    const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
    GAMENET_TEST_ASSERT(summary.listenerStoppedBeforeWorkers);
    GAMENET_TEST_ASSERT(summary.allHandoffsSettled);
    GAMENET_TEST_ASSERT(summary.allWorkersStopped);
    GAMENET_TEST_ASSERT(summary.server.pendingHandoffs == 0);
    GAMENET_TEST_ASSERT(summary.server.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.acceptHub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.acceptHub.listener.has_value());
    GAMENET_TEST_ASSERT(summary.acceptHub.listener->socketClosed);
    GAMENET_TEST_ASSERT(summary.acceptHub.listener->acceptsRetired);
    GAMENET_TEST_ASSERT(
        summary.acceptHub.listener->listener.acceptedSockets ==
        summary.acceptHub.listener->listener.acceptedSocketHandoffs +
            summary.acceptHub.listener->listener.acceptedSocketRejections);
    GAMENET_TEST_ASSERT(
        summary.acceptHub.listener->listener.listenerSocketCloseCount == 1);
    GAMENET_TEST_ASSERT(summary.acceptHub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.acceptHub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.acceptHub.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.acceptHub.pump.engine.ownedBytes == 0);
    for (const auto& worker : summary.workers) {
        GAMENET_TEST_ASSERT(worker.ownerDestroyedHub);
        GAMENET_TEST_ASSERT(worker.activeConnections == 0);
        GAMENET_TEST_ASSERT(worker.hub.allConnectionsStopped);
        GAMENET_TEST_ASSERT(!worker.hub.listener.has_value());
        GAMENET_TEST_ASSERT(worker.hub.hub.activeConnections == 0);
        GAMENET_TEST_ASSERT(worker.hub.hub.activeOperationRoutes == 0);
        GAMENET_TEST_ASSERT(worker.hub.hub.pendingSendBytes == 0);
        GAMENET_TEST_ASSERT(worker.hub.pump.engine.activeOperations == 0);
        GAMENET_TEST_ASSERT(worker.hub.pump.engine.readyNotices == 0);
        GAMENET_TEST_ASSERT(worker.hub.pump.engine.ownedBytes == 0);
        GAMENET_TEST_ASSERT(
            worker.hub.hub.socketCloseCount ==
            worker.connectionsEstablished);
    }
}

void testRoundRobinHandoffKeepsWorkerOwnership() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    std::mutex observationsMutex;
    std::vector<std::size_t> establishedWorkers;
    std::array<std::size_t, 4> payloadWorkers{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    std::vector<gamenet::net::EventLoopExecutor> executors;
    std::atomic<std::size_t> echoed{0};

    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        serverOptions(gamenet::net::EventLoopSelectionPolicy::RoundRobin),
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            GAMENET_TEST_ASSERT(acceptLoop.isInLoopThread());
            stopped = summary;
            acceptLoop.quit();
        });
    server.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t worker) {
            GAMENET_TEST_ASSERT(worker < executors.size());
            GAMENET_TEST_ASSERT(executors[worker].isInOwnerThread());
            GAMENET_TEST_ASSERT(!acceptLoop.isInLoopThread());
            if (!connection.connected()) return;
            std::lock_guard lock(observationsMutex);
            establishedWorkers.push_back(worker);
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t worker,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(executors[worker].isInOwnerThread());
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
            GAMENET_TEST_ASSERT(payload.size() == 8);
            const auto payloadIndex =
                static_cast<std::size_t>(payload.back() - '0');
            GAMENET_TEST_ASSERT(payloadIndex < payloadWorkers.size());
            {
                std::lock_guard lock(observationsMutex);
                payloadWorkers[payloadIndex] = worker;
            }
            echoed.fetch_add(1, std::memory_order_release);
        });

    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    executors = server.workerExecutors();
    GAMENET_TEST_ASSERT(executors.size() == 2);

    std::vector<OwnedFd> clients;
    std::vector<std::string> replies(4);
    for (std::size_t index = 0; index < 4; ++index) {
        clients.push_back(connectClient(start.listenAddress));
        sendAll(clients.back().get(), "x12-rr-" + std::to_string(index));
    }
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        bool complete = echoed.load(std::memory_order_acquire) == clients.size();
        for (std::size_t index = 0; index < clients.size(); ++index) {
            (void)drainAvailable(clients[index], replies[index]);
            complete = complete && replies[index] ==
                "x12-rr-" + std::to_string(index);
        }
        if (complete && !stopRequested) {
            stopRequested = true;
            for (auto& client : clients) client.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 RoundRobin handoff timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);

    {
        std::lock_guard lock(observationsMutex);
        GAMENET_TEST_ASSERT(establishedWorkers.size() == 4);
        const std::array<std::size_t, 4> expected{0, 1, 0, 1};
        GAMENET_TEST_ASSERT(payloadWorkers == expected);
    }
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.handoffsAccepted == 4);
    GAMENET_TEST_ASSERT(stopped->server.connectionsEstablished == 4);
    GAMENET_TEST_ASSERT(stopped->server.connectionsRetired == 4);
    assertZeroResidue(*stopped);
}

void testLeastConnectionsUsesReservedAndPhysicalLoads() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    std::mutex observationsMutex;
    std::array<std::size_t, 3> payloadWorkers{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    std::vector<gamenet::net::EventLoopExecutor> executors;
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        serverOptions(
            gamenet::net::EventLoopSelectionPolicy::LeastConnections),
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t worker,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(executors[worker].isInOwnerThread());
            const auto payloadIndex =
                static_cast<std::size_t>(payload.back() - '0');
            GAMENET_TEST_ASSERT(payloadIndex < payloadWorkers.size());
            {
                std::lock_guard lock(observationsMutex);
                payloadWorkers[payloadIndex] = worker;
            }
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    executors = server.workerExecutors();

    std::array<std::optional<OwnedFd>, 3> clients;
    std::array<std::string, 3> replies;
    clients[0].emplace(connectClient(start.listenAddress));
    sendAll(clients[0]->get(), "least-0");
    bool secondStarted = false;
    bool firstClosed = false;
    bool thirdStarted = false;
    bool gracefulStarted = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        for (std::size_t index = 0; index < clients.size(); ++index) {
            if (clients[index]) {
                (void)drainAvailable(*clients[index], replies[index]);
            }
        }
        if (!secondStarted && replies[0] == "least-0") {
            secondStarted = true;
            clients[1].emplace(connectClient(start.listenAddress));
            sendAll(clients[1]->get(), "least-1");
        }
        if (!firstClosed && replies[1] == "least-1") {
            firstClosed = true;
            clients[0]->close();
        }
        if (!thirdStarted && firstClosed &&
            server.metrics().connectionsRetired >= 1) {
            thirdStarted = true;
            clients[2].emplace(connectClient(start.listenAddress));
            sendAll(clients[2]->get(), "least-2");
        }
        if (!gracefulStarted && replies[2] == "least-2") {
            gracefulStarted = true;
            (void)server.stopGracefully();
        }
        if (gracefulStarted) {
            for (auto& client : clients) {
                if (client && !client->valid()) client.reset();
            }
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 LeastConnections timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);

    {
        std::lock_guard lock(observationsMutex);
        const std::array<std::size_t, 3> expected{0, 1, 0};
        GAMENET_TEST_ASSERT(payloadWorkers == expected);
    }
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.gracefulStopRequests == 1);
    assertZeroResidue(*stopped);
}

void testQueueLagAvoidsWorkerWithOlderPendingWork() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    std::atomic<bool> releaseBlockedWorker{false};
    std::promise<void> blockerEnteredPromise;
    auto blockerEntered = blockerEnteredPromise.get_future();
    std::atomic<std::size_t> selectedWorker{
        std::numeric_limits<std::size_t>::max()};
    auto options = serverOptions(
        gamenet::net::EventLoopSelectionPolicy::QueueLag);
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t worker,
            std::string_view payload) {
            selectedWorker.store(worker, std::memory_order_release);
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    const auto executors = server.workerExecutors();
    GAMENET_TEST_ASSERT(executors.size() == 2);
    GAMENET_TEST_ASSERT(
        executors[0].post([&] {
            blockerEnteredPromise.set_value();
            while (!releaseBlockedWorker.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(blockerEntered.wait_for(2s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(
        executors[0].post([] {}) == gamenet::net::PostResult::Accepted);

    auto client = connectClient(start.listenAddress);
    sendAll(client.get(), "queue-lag");
    std::string reply;
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        (void)drainAvailable(client, reply);
        if (!stopRequested && reply == "queue-lag") {
            stopRequested = true;
            GAMENET_TEST_ASSERT(
                selectedWorker.load(std::memory_order_acquire) == 1);
            releaseBlockedWorker.store(true, std::memory_order_release);
            client.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [&] {
        releaseBlockedWorker.store(true, std::memory_order_release);
        GAMENET_TEST_FAIL("IOE-X12 QueueLag timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    GAMENET_TEST_ASSERT(stopped.has_value());
    assertZeroResidue(*stopped);
}

void testConsistentHashKeepsPeerIpStable() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    std::mutex observationsMutex;
    std::vector<std::size_t> workers;
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        serverOptions(
            gamenet::net::EventLoopSelectionPolicy::ConsistentHash),
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t worker,
            std::string_view payload) {
            {
                std::lock_guard lock(observationsMutex);
                workers.push_back(worker);
            }
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    std::vector<OwnedFd> clients;
    std::vector<std::string> replies(4);
    for (std::size_t index = 0; index < replies.size(); ++index) {
        clients.push_back(connectClient(start.listenAddress));
        sendAll(clients.back().get(), "hash-" + std::to_string(index));
    }
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        bool complete = true;
        for (std::size_t index = 0; index < replies.size(); ++index) {
            (void)drainAvailable(clients[index], replies[index]);
            complete = complete &&
                replies[index] == "hash-" + std::to_string(index);
        }
        if (complete && !stopRequested) {
            stopRequested = true;
            for (auto& client : clients) client.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 ConsistentHash timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    {
        std::lock_guard lock(observationsMutex);
        GAMENET_TEST_ASSERT(workers.size() == 4);
        GAMENET_TEST_ASSERT(std::all_of(
            workers.begin(), workers.end(), [&](std::size_t worker) {
                return worker == workers.front();
            }));
    }
    GAMENET_TEST_ASSERT(stopped.has_value());
    assertZeroResidue(*stopped);
}

void testBoundedHandoffRejectsWithoutOverflowQueue() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    auto options = serverOptions(
        gamenet::net::EventLoopSelectionPolicy::RoundRobin);
    options.workerCount = 1;
    options.maxPendingHandoffs = 1;
    std::atomic<bool> releaseWorker{false};
    std::promise<void> blockerEnteredPromise;
    auto blockerEntered = blockerEnteredPromise.get_future();
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    const auto executor = server.workerExecutors().front();
    GAMENET_TEST_ASSERT(
        executor.post([&] {
            blockerEnteredPromise.set_value();
            while (!releaseWorker.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(blockerEntered.wait_for(2s) == std::future_status::ready);
    auto first = connectClient(start.listenAddress);
    auto rejected = connectClient(start.listenAddress);
    bool released = false;
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        const auto snapshot = server.metrics();
        if (!released && snapshot.handoffsAccepted == 1 &&
            snapshot.handoffLimitRejections >= 1) {
            released = true;
            releaseWorker.store(true, std::memory_order_release);
        }
        if (released && !stopRequested &&
            snapshot.connectionsEstablished == 1) {
            stopRequested = true;
            first.close();
            rejected.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [&] {
        releaseWorker.store(true, std::memory_order_release);
        GAMENET_TEST_FAIL("IOE-X12 bounded handoff timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.handoffsAccepted == 1);
    GAMENET_TEST_ASSERT(stopped->server.handoffLimitRejections >= 1);
    GAMENET_TEST_ASSERT(stopped->server.pendingHandoffs == 0);
    assertZeroResidue(*stopped);
}

void testEventLoopQueueFullRetainsAcceptOwnerUntilClose() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    auto options = serverOptions(
        gamenet::net::EventLoopSelectionPolicy::RoundRobin);
    options.workerCount = 1;
    options.maxPendingHandoffs = 2;
    std::atomic<bool> releaseWorker{false};
    std::promise<void> blockerEnteredPromise;
    auto blockerEntered = blockerEnteredPromise.get_future();
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    const auto executor = server.workerExecutors().front();
    GAMENET_TEST_ASSERT(
        executor.post([&] {
            blockerEnteredPromise.set_value();
            while (!releaseWorker.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(blockerEntered.wait_for(2s) == std::future_status::ready);
    std::size_t queued = 0;
    while (executor.post([] {}) == gamenet::net::PostResult::Accepted) {
        ++queued;
    }
    GAMENET_TEST_ASSERT(queued == 65536);
    auto rejected = connectClient(start.listenAddress);
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        const auto snapshot = server.metrics();
        if (!stopRequested && snapshot.handoffQueueRejections >= 1) {
            stopRequested = true;
            releaseWorker.store(true, std::memory_order_release);
            rejected.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [&] {
        releaseWorker.store(true, std::memory_order_release);
        GAMENET_TEST_FAIL("IOE-X12 EventLoop QueueFull timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.handoffsAccepted == 0);
    GAMENET_TEST_ASSERT(stopped->server.handoffQueueRejections >= 1);
    GAMENET_TEST_ASSERT(stopped->server.handoffLimitRejections == 0);
    assertZeroResidue(*stopped);
}

void testWorkerHubAdmissionFailureRollsBackPlacementLoad() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    auto options = serverOptions(
        gamenet::net::EventLoopSelectionPolicy::RoundRobin);
    options.workerCount = 1;
    options.workerHub.maxConnections = 2;
    options.maxPendingHandoffs = 4;
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    std::array<OwnedFd, 3> clients{
        connectClient(start.listenAddress),
        connectClient(start.listenAddress),
        connectClient(start.listenAddress)};
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        const auto snapshot = server.metrics();
        if (!stopRequested && snapshot.connectionsEstablished == 2 &&
            snapshot.workerAdmissionRejections >= 1) {
            stopRequested = true;
            for (auto& client : clients) client.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 worker admission rollback timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.handoffsAccepted == 3);
    GAMENET_TEST_ASSERT(stopped->server.connectionsEstablished == 2);
    GAMENET_TEST_ASSERT(stopped->server.workerAdmissionRejections >= 1);
    assertZeroResidue(*stopped);
}

void testAcceptOwnerQuitForcesAllWorkerResidueToZero() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        serverOptions(gamenet::net::EventLoopSelectionPolicy::RoundRobin),
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
        });
    server.setMessageCallback(
        [](uring::IoUringTcpConnectionAdapter& connection,
           std::size_t,
           std::string_view payload) {
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    auto client = connectClient(start.listenAddress);
    sendAll(client.get(), "owner-quit");
    std::string reply;
    bool quitRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        (void)drainAvailable(client, reply);
        if (!quitRequested && reply == "owner-quit") {
            quitRequested = true;
            acceptLoop.quit();
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 accept-owner quit timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    client.close();
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.forceStopRequests == 1);
    assertZeroResidue(*stopped);
}

void testWorkerShutdownRejectsNewAcceptedSocket() {
    gamenet::net::EventLoop acceptLoop;
    std::optional<uring::IoUringTcpMultiOwnerServerStopSummary> stopped;
    std::vector<gamenet::net::EventLoop*> workerLoops;
    auto options = serverOptions(
        gamenet::net::EventLoopSelectionPolicy::RoundRobin);
    options.workerCount = 1;
    uring::IoUringTcpMultiOwnerServer server(
        &acceptLoop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpMultiOwnerServerStopSummary& summary) {
            stopped = summary;
            acceptLoop.quit();
        });
    server.setWorkerInitCallback(
        [&](gamenet::net::EventLoop* loop) { workerLoops.push_back(loop); });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    const auto executor = server.workerExecutors().front();
    GAMENET_TEST_ASSERT(workerLoops.size() == 1);
    std::promise<void> quitIssuedPromise;
    auto quitIssued = quitIssuedPromise.get_future();
    GAMENET_TEST_ASSERT(
        executor.post([&, workerLoop = workerLoops.front()] {
            workerLoop->quit();
            quitIssuedPromise.set_value();
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(quitIssued.wait_for(2s) == std::future_status::ready);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (executor.available() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(!executor.available());
    auto rejected = connectClient(start.listenAddress);
    bool stopRequested = false;
    const auto progress = acceptLoop.runEvery(1ms, [&] {
        if (!stopRequested &&
            server.metrics().handoffShutdownRejections >= 1) {
            stopRequested = true;
            rejected.close();
            (void)server.forceStop();
        }
    });
    acceptLoop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("IOE-X12 worker-shutdown rejection timed out");
    });
    acceptLoop.loop();
    acceptLoop.cancel(progress);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.handoffsAccepted == 0);
    GAMENET_TEST_ASSERT(stopped->server.handoffShutdownRejections >= 1);
    assertZeroResidue(*stopped);
}

}  // namespace

int main() {
    testRoundRobinHandoffKeepsWorkerOwnership();
    testLeastConnectionsUsesReservedAndPhysicalLoads();
    testQueueLagAvoidsWorkerWithOlderPendingWork();
    testConsistentHashKeepsPeerIpStable();
    testBoundedHandoffRejectsWithoutOverflowQueue();
    testEventLoopQueueFullRetainsAcceptOwnerUntilClose();
    testWorkerHubAdmissionFailureRollsBackPlacementLoad();
    testWorkerShutdownRejectsNewAcceptedSocket();
    testAcceptOwnerQuitForcesAllWorkerResidueToZero();
    return 0;
}
