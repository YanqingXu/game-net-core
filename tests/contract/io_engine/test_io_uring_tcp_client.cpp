// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/experimental/io_uring/IoUringTcpClient.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
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

class LoopbackEchoPeer {
public:
    LoopbackEchoPeer() {
        listener_ = OwnedFd(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            IPPROTO_TCP));
        GAMENET_TEST_ASSERT(listener_.valid());
        int reuse = 1;
        GAMENET_TEST_ASSERT(
            ::setsockopt(
                listener_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse)) == 0);
        sockaddr_in native{};
        native.sin_family = AF_INET;
        native.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        native.sin_port = 0;
        GAMENET_TEST_ASSERT(
            ::bind(
                listener_.get(),
                reinterpret_cast<const sockaddr*>(&native),
                sizeof(native)) == 0);
        socklen_t length = sizeof(native);
        GAMENET_TEST_ASSERT(
            ::getsockname(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&native),
                &length) == 0);
        address_ = gamenet::net::InetAddress(native);
    }

    ~LoopbackEchoPeer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        listener_.close();
    }

    const gamenet::net::InetAddress& address() const noexcept {
        return address_;
    }

    void start(std::size_t maximumConnections = 8) {
        GAMENET_TEST_ASSERT(!thread_.joinable());
        GAMENET_TEST_ASSERT(::listen(listener_.get(), 16) == 0);
        thread_ = std::thread([this, maximumConnections] {
            run(maximumConnections);
        });
    }

    std::size_t acceptedConnections() const noexcept {
        return accepted_.load(std::memory_order_acquire);
    }

    std::string received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }

private:
    void run(std::size_t maximumConnections) noexcept {
        while (!stop_.load(std::memory_order_acquire) &&
               accepted_.load(std::memory_order_acquire) <
                   maximumConnections) {
            pollfd descriptor{
                .fd = listener_.get(),
                .events = POLLIN,
                .revents = 0,
            };
            const auto ready = ::poll(&descriptor, 1, 20);
            if (ready <= 0) continue;
            OwnedFd peer(::accept4(
                listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!peer.valid()) continue;
            accepted_.fetch_add(1, std::memory_order_acq_rel);
            drivePeer(peer);
        }
    }

    void drivePeer(OwnedFd& peer) noexcept {
        char buffer[256]{};
        while (!stop_.load(std::memory_order_acquire) && peer.valid()) {
            pollfd descriptor{
                .fd = peer.get(),
                .events = POLLIN,
                .revents = 0,
            };
            const auto ready = ::poll(&descriptor, 1, 20);
            if (ready <= 0) continue;
            const auto count = ::recv(peer.get(), buffer, sizeof(buffer), 0);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }
            {
                std::lock_guard lock(mutex_);
                received_.append(buffer, static_cast<std::size_t>(count));
            }
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                const auto sent = ::send(
                    peer.get(),
                    buffer + offset,
                    static_cast<std::size_t>(count) - offset,
                    MSG_NOSIGNAL);
                if (sent > 0) {
                    offset += static_cast<std::size_t>(sent);
                } else if (sent < 0 &&
                           (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
                    std::this_thread::yield();
                } else {
                    peer.close();
                    break;
                }
            }
        }
    }

    OwnedFd listener_;
    gamenet::net::InetAddress address_;
    mutable std::mutex mutex_;
    std::string received_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> accepted_{0};
};

uring::IoUringTcpClientOptions clientOptions() {
    return {
        .hub = {
            .pump = {
                .engine = {
                    .entries = 32,
                    .maxOperations = 16,
                    .maxCompletionsPerWait = 16,
                    .maxBytesPerOperation = 32,
                    .maxOwnedBytes = 256,
                },
                .maxNoticesPerTurn = 4,
            },
            .maxConnections = 2,
            .maxTotalPendingSendBytes = 128,
            .maxReceiveBytes = 32,
            .maxSendBytesPerOperation = 32,
            .maxPendingSendBytesPerConnection = 64,
            .maxPendingSendSegmentsPerConnection = 4,
            .maxPendingAccepts = 1,
        },
        .connection = {
            .lowWaterMarkBytes = 8,
            .highWaterMarkBytes = 16,
            .hardLimitBytes = 64,
            .maxPendingCommands = 8,
            .maxCommandsPerTurn = 4,
        },
        .initialRetryDelay = 5ms,
        .maximumRetryDelay = 20ms,
        .connectTimeout = std::nullopt,
        .retryEnabled = false,
    };
}

void assertZeroResidue(const uring::IoUringTcpClientStopSummary& summary) {
    GAMENET_TEST_ASSERT(summary.allTimersRetired);
    GAMENET_TEST_ASSERT(summary.allAttemptsRetired);
    GAMENET_TEST_ASSERT(summary.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.ownerDestroyedHub);
    GAMENET_TEST_ASSERT(summary.client.activeAttempts == 0);
    GAMENET_TEST_ASSERT(summary.client.activeConnections == 0);
    GAMENET_TEST_ASSERT(!summary.client.retryTimerPending);
    GAMENET_TEST_ASSERT(!summary.client.timeoutTimerPending);
    GAMENET_TEST_ASSERT(summary.client.hubDestroyed);
    GAMENET_TEST_ASSERT(
        summary.client.attemptSocketsCreated ==
        summary.client.attemptSocketsClosed +
            summary.client.attemptSocketsTransferred);
    GAMENET_TEST_ASSERT(summary.hub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.invariantFailures == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.pendingSubmissions == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.ownedBytes == 0);
}

std::vector<std::string> runProductionObservationSequence() {
    LoopbackEchoPeer peer;
    peer.start(1);
    gamenet::net::EventLoop loop;
    gamenet::net::TcpClient client(
        &loop, peer.address(), "x13-production-observer");
    std::vector<std::string> sequence;
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                sequence.emplace_back("connected");
                connection->send("production");
            } else {
                sequence.emplace_back("disconnected");
                client.stop();
                loop.quit();
            }
        });
    client.setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* buffer) {
            GAMENET_TEST_ASSERT(buffer->retrieveAllAsString() == "production");
            sequence.emplace_back("message");
            connection->shutdown();
        });
    client.connect();
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("production TcpClient comparison timed out");
    });
    loop.loop();
    return sequence;
}

void testConnectSuccessEchoAndProductionObservationOrder() {
    const auto production = runProductionObservationSequence();
    GAMENET_TEST_ASSERT(
        production ==
        std::vector<std::string>({"connected", "message", "disconnected"}));

    LoopbackEchoPeer peer;
    peer.start(1);
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    std::vector<std::string> sequence;
    std::vector<gamenet::net::ConnectorEvent> events;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        clientOptions(),
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.push_back(event);
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (connection.connected()) {
                sequence.emplace_back("connected");
                GAMENET_TEST_ASSERT(
                    connection.trySend("experimental") ==
                    gamenet::net::TcpSendResult::Accepted);
            } else {
                sequence.emplace_back("disconnected");
                (void)client.stop();
            }
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "experimental");
            sequence.emplace_back("message");
            GAMENET_TEST_ASSERT(
                connection.tryShutdown() == gamenet::net::PostResult::Accepted);
        });
    std::atomic<std::size_t> foreignRejections{0};
    std::thread foreign([&] {
        const auto expectOwnerRejection = [&](auto&& action) {
            try {
                action();
            } catch (const std::runtime_error&) {
                foreignRejections.fetch_add(1, std::memory_order_relaxed);
            }
        };
        expectOwnerRejection([&] { (void)client.phase(); });
        expectOwnerRejection([&] { (void)client.stopFuture(); });
        expectOwnerRejection([&] { (void)client.connect(); });
    });
    foreign.join();
    GAMENET_TEST_ASSERT(
        foreignRejections.load(std::memory_order_relaxed) == 3);
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 success Client timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(sequence == production);
    GAMENET_TEST_ASSERT(events.size() >= 2);
    GAMENET_TEST_ASSERT(
        events[0] == gamenet::net::ConnectorEvent::ConnectAttempt);
    GAMENET_TEST_ASSERT(
        events[1] == gamenet::net::ConnectorEvent::ConnectSuccess);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->client.connectSuccesses == 1);
    GAMENET_TEST_ASSERT(stopped->client.connectionsEstablished == 1);
    GAMENET_TEST_ASSERT(stopped->client.connectionsRetired == 1);
    GAMENET_TEST_ASSERT(stopped->hub.connect.has_value());
    GAMENET_TEST_ASSERT(
        stopped->hub.connect->reason ==
        uring::IoUringTcpHubConnectCloseReason::Connected);
    assertZeroResidue(*stopped);
}

void testRefusedAttemptRetriesAfterListenerStarts() {
    LoopbackEchoPeer peer;
    auto options = clientOptions();
    options.retryEnabled = true;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    std::vector<gamenet::net::ConnectorEvent> events;
    bool listenerStarted = false;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        options,
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            events.push_back(event);
            if (event == gamenet::net::ConnectorEvent::RetryScheduled &&
                !listenerStarted) {
                listenerStarted = true;
                peer.start(1);
            }
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (connection.connected()) {
                GAMENET_TEST_ASSERT(
                    connection.trySend("retry") ==
                    gamenet::net::TcpSendResult::Accepted);
            }
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter&, std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "retry");
            (void)client.stop();
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 retry Client timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(listenerStarted);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->client.attemptsStarted >= 2);
    GAMENET_TEST_ASSERT(stopped->client.connectFailures >= 1);
    GAMENET_TEST_ASSERT(stopped->client.retriesScheduled >= 1);
    GAMENET_TEST_ASSERT(stopped->client.connectSuccesses == 1);
    GAMENET_TEST_ASSERT(
        std::find(
            events.begin(),
            events.end(),
            gamenet::net::ConnectorEvent::ConnectFailed) != events.end());
    GAMENET_TEST_ASSERT(
        std::find(
            events.begin(),
            events.end(),
            gamenet::net::ConnectorEvent::RetryScheduled) != events.end());
    assertZeroResidue(*stopped);
}

void testImmediateTimeoutCancelsExactAttempt() {
    LoopbackEchoPeer reservedButNotListening;
    auto options = clientOptions();
    options.connectTimeout = 0ms;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    std::vector<gamenet::net::ConnectorEvent> events;
    uring::IoUringTcpClient client(
        &loop,
        reservedButNotListening.address(),
        options,
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            events.push_back(event);
            if (event == gamenet::net::ConnectorEvent::TerminalFailure) {
                (void)client.stop();
            }
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 immediate timeout timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->client.connectTimeouts == 1);
    GAMENET_TEST_ASSERT(stopped->client.connectSuccesses == 0);
    GAMENET_TEST_ASSERT(stopped->hub.connect.has_value());
    GAMENET_TEST_ASSERT(
        stopped->hub.connect->reason ==
        uring::IoUringTcpHubConnectCloseReason::Timeout);
    GAMENET_TEST_ASSERT(
        std::find(
            events.begin(),
            events.end(),
            gamenet::net::ConnectorEvent::ConnectTimeout) != events.end());
    assertZeroResidue(*stopped);
}

void testRestartCancelsStaleAttemptBeforePublication() {
    LoopbackEchoPeer peer;
    peer.start(2);
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    std::size_t connectedCallbacks = 0;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        clientOptions(),
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (!connection.connected()) return;
            ++connectedCallbacks;
            GAMENET_TEST_ASSERT(
                connection.trySend("replacement") ==
                gamenet::net::TcpSendResult::Accepted);
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter&, std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "replacement");
            (void)client.stop();
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    GAMENET_TEST_ASSERT(
        client.restart().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 stale restart timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->client.staleTerminals >= 1);
    GAMENET_TEST_ASSERT(stopped->client.connectCancellations >= 1);
    GAMENET_TEST_ASSERT(stopped->client.attemptsStarted == 2);
    GAMENET_TEST_ASSERT(stopped->client.connectSuccesses == 1);
    GAMENET_TEST_ASSERT(connectedCallbacks == 1);
    assertZeroResidue(*stopped);
}

void testTerminalFailureCallbackStartsFreshLifecycle() {
    LoopbackEchoPeer peer;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    bool freshConnectAccepted = false;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        clientOptions(),
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            if (event == gamenet::net::ConnectorEvent::TerminalFailure &&
                !freshConnectAccepted) {
                peer.start(1);
                freshConnectAccepted =
                    client.connect().result ==
                    uring::IoUringTcpClientConnectResult::Accepted;
            }
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (connection.connected()) {
                GAMENET_TEST_ASSERT(
                    connection.trySend("fresh") ==
                    gamenet::net::TcpSendResult::Accepted);
            }
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter&, std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "fresh");
            (void)client.stop();
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 reentrant terminal failure timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(freshConnectAccepted);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->client.attemptsStarted == 2);
    GAMENET_TEST_ASSERT(stopped->client.connectFailures == 1);
    GAMENET_TEST_ASSERT(stopped->client.connectSuccesses == 1);
    assertZeroResidue(*stopped);
}

void testConnectSuccessCallbackRestartSuppressesOldPublication() {
    LoopbackEchoPeer peer;
    peer.start(2);
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    std::size_t successes = 0;
    std::size_t connectedCallbacks = 0;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        clientOptions(),
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    client.setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            if (event != gamenet::net::ConnectorEvent::ConnectSuccess) return;
            ++successes;
            if (successes == 1) {
                GAMENET_TEST_ASSERT(
                    client.restart().result ==
                    uring::IoUringTcpClientConnectResult::Accepted);
            }
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (!connection.connected()) return;
            ++connectedCallbacks;
            GAMENET_TEST_ASSERT(
                connection.trySend("reentrant-success") ==
                gamenet::net::TcpSendResult::Accepted);
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter&, std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "reentrant-success");
            (void)client.stop();
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 reentrant ConnectSuccess timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(successes == 2);
    GAMENET_TEST_ASSERT(connectedCallbacks == 1);
    GAMENET_TEST_ASSERT(stopped->client.connectionsEstablished == 2);
    GAMENET_TEST_ASSERT(stopped->client.connectionsRetired == 2);
    assertZeroResidue(*stopped);
}

void testOwnerQuitRetiresEstablishedClientBeforeShutdown() {
    LoopbackEchoPeer peer;
    peer.start(1);
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpClientStopSummary> stopped;
    uring::IoUringTcpClient client(
        &loop,
        peer.address(),
        clientOptions(),
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            stopped = summary;
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            if (connection.connected()) loop.quit();
        });
    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X13 owner quit timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(
        client.phase() == uring::IoUringTcpClientPhase::Stopped);
    assertZeroResidue(*stopped);
}

}  // namespace

int main() {
    testConnectSuccessEchoAndProductionObservationOrder();
    testRefusedAttemptRetriesAfterListenerStarts();
    testImmediateTimeoutCancelsExactAttempt();
    testRestartCancelsStaleAttemptBeforePublication();
    testTerminalFailureCallbackStartsFreshLifecycle();
    testConnectSuccessCallbackRestartSuppressesOldPublication();
    testOwnerQuitRetiresEstablishedClientBeforeShutdown();
    return 0;
}
