// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "experimental/io_uring/IoUringTcpServer.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
        if (value_ < 0) return;
        ::close(value_);
        value_ = -1;
    }

private:
    int value_;
};

OwnedFd connectClient(const gamenet::net::InetAddress& address) {
    GAMENET_TEST_ASSERT(address.isIpv4());
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
            GAMENET_TEST_FAIL("failed to send test payload");
        }
    }
}

bool drainAvailable(OwnedFd& client, std::string& output) {
    std::array<char, 64> buffer{};
    while (client.valid()) {
        const auto count = ::recv(
            client.get(),
            buffer.data(),
            buffer.size(),
            0);
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
        GAMENET_TEST_FAIL("unexpected client receive failure");
    }
    return true;
}

uring::IoUringTcpServerOptions serverOptions() {
    return {
        .hub = {
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
        .connection = {
            .lowWaterMarkBytes = 4,
            .highWaterMarkBytes = 8,
            .hardLimitBytes = 32,
            .maxPendingCommands = 8,
            .maxCommandsPerTurn = 4,
        },
        .reuseAddress = true,
        .reusePort = false,
    };
}

void assertZeroResidue(const uring::IoUringTcpServerStopSummary& summary) {
    GAMENET_TEST_ASSERT(summary.listenerStoppedBeforeHub);
    GAMENET_TEST_ASSERT(summary.allAdaptersRetired);
    GAMENET_TEST_ASSERT(summary.server.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.server.provisionalConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.hub.listener.has_value());
    GAMENET_TEST_ASSERT(summary.hub.listener->socketClosed);
    GAMENET_TEST_ASSERT(summary.hub.listener->acceptsRetired);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.invariantFailures == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.ownedBytes == 0);
}

void testServerBindEchoAndGracefulStop() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    std::optional<gamenet::net::TcpConnectionCloseInfo> closeInfo;
    std::size_t connectionCallbacks = 0;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        serverOptions(),
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            stopped = summary;
            loop.quit();
        });
    server.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++connectionCallbacks;
            if (connectionCallbacks == 1) {
                GAMENET_TEST_ASSERT(connection.connected());
            } else {
                GAMENET_TEST_ASSERT(connection.disconnected());
            }
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
            (void)server.stopGracefully();
        });
    server.setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            closeInfo = info;
        });

    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    GAMENET_TEST_ASSERT(start.listenAddress.port() != 0);
    GAMENET_TEST_ASSERT(server.listenAddress().port() == start.listenAddress.port());
    auto client = connectClient(start.listenAddress);
    constexpr std::string_view payload = "x11-echo";
    sendAll(client.get(), payload);
    std::string echoed;
    const auto progress = loop.runEvery(1ms, [&] {
        (void)drainAvailable(client, echoed);
    });
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X11 graceful Server timed out");
    });
    loop.loop();
    loop.cancel(progress);

    GAMENET_TEST_ASSERT(echoed == payload);
    GAMENET_TEST_ASSERT(connectionCallbacks == 2);
    GAMENET_TEST_ASSERT(closeInfo.has_value());
    GAMENET_TEST_ASSERT(
        closeInfo->reason ==
        gamenet::net::TcpConnectionCloseReason::GracefulShutdown);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.connectionsEstablished == 1);
    GAMENET_TEST_ASSERT(stopped->server.connectionsRetired == 1);
    GAMENET_TEST_ASSERT(stopped->server.gracefulStopRequests == 1);
    assertZeroResidue(*stopped);
    GAMENET_TEST_ASSERT(
        server.phase() == uring::IoUringTcpServerPhase::Stopped);
}

void testCallbackReentryForceEscalationPreservesFirstReason() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    std::optional<gamenet::net::TcpConnectionCloseInfo> closeInfo;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        serverOptions(),
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                connection.trySend("accepted-before-stop") ==
                gamenet::net::TcpSendResult::Accepted);
            (void)server.stopGracefully();
            (void)server.forceStop();
        });
    server.setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            closeInfo = info;
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    auto client = connectClient(start.listenAddress);
    sendAll(client.get(), "force");
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X11 force escalation timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(closeInfo.has_value());
    GAMENET_TEST_ASSERT(
        closeInfo->reason ==
        gamenet::net::TcpConnectionCloseReason::GracefulShutdown);
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.forceEscalated);
    GAMENET_TEST_ASSERT(stopped->server.gracefulStopRequests == 1);
    GAMENET_TEST_ASSERT(stopped->server.forceStopRequests == 1);
    assertZeroResidue(*stopped);
}

void testForeignObservationAndMutationRejectBeforeOwnerStateRead() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        serverOptions(),
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    GAMENET_TEST_ASSERT(
        server.start().result ==
        uring::IoUringTcpServerStartResult::Accepted);
    std::atomic<bool> rejected{false};
    std::atomic<bool> mutationRejected{false};
    std::thread foreign([&] {
        try {
            (void)server.connectionCount();
        } catch (const std::runtime_error&) {
            rejected.store(true, std::memory_order_release);
        }
        try {
            (void)server.stopGracefully();
        } catch (const std::runtime_error&) {
            mutationRejected.store(true, std::memory_order_release);
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(rejected.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(mutationRejected.load(std::memory_order_acquire));
    (void)server.forceStop();
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X11 foreign-observation stop timed out");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped.has_value());
    assertZeroResidue(*stopped);
}

void testConnectionLimitSettlesProvisionalAdapter() {
    auto options = serverOptions();
    options.hub.maxConnections = 2;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        options,
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    auto first = connectClient(start.listenAddress);
    auto second = connectClient(start.listenAddress);
    auto rejected = connectClient(start.listenAddress);
    bool stopRequested = false;
    const auto progress = loop.runEvery(1ms, [&] {
        const auto metrics = server.metrics();
        if (!stopRequested && metrics.connectionsEstablished == 2 &&
            metrics.connectionAdmissionRejections >= 1) {
            GAMENET_TEST_ASSERT(metrics.provisionalConnections == 0);
            stopRequested = true;
            (void)server.forceStop();
        }
    });
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("IOE-X11 connection-limit settlement timed out");
    });
    loop.loop();
    loop.cancel(progress);

    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.connectionsEstablished == 2);
    GAMENET_TEST_ASSERT(
        stopped->server.connectionAdmissionRejections >= 1);
    GAMENET_TEST_ASSERT(stopped->server.provisionalConnections == 0);
    assertZeroResidue(*stopped);
}

void testBindFailureIsTypedAndStillStopsPhysically() {
    OwnedFd occupied(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_TCP));
    GAMENET_TEST_ASSERT(occupied.valid());
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    native.sin_port = 0;
    GAMENET_TEST_ASSERT(
        ::bind(
            occupied.get(),
            reinterpret_cast<const sockaddr*>(&native),
            sizeof(native)) == 0);
    GAMENET_TEST_ASSERT(::listen(occupied.get(), 8) == 0);
    socklen_t length = sizeof(native);
    GAMENET_TEST_ASSERT(
        ::getsockname(
            occupied.get(),
            reinterpret_cast<sockaddr*>(&native),
            &length) == 0);

    auto options = serverOptions();
    options.reuseAddress = false;
    options.reusePort = false;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(native),
        options,
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            stopped = summary;
            loop.quit();
        });
    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::BindFailed);
    GAMENET_TEST_ASSERT(start.nativeError == EADDRINUSE);
    if (server.stopFuture().wait_for(0ms) != std::future_status::ready) {
        loop.runAfter(4s, [] {
            GAMENET_TEST_FAIL("IOE-X11 bind-failure stop timed out");
        });
        loop.loop();
    }
    GAMENET_TEST_ASSERT(stopped.has_value());
    GAMENET_TEST_ASSERT(stopped->server.bindFailures == 1);
    GAMENET_TEST_ASSERT(stopped->allAdaptersRetired);
    GAMENET_TEST_ASSERT(stopped->hub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(stopped->hub.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(stopped->hub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(stopped->hub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(stopped->hub.pump.engine.ownedBytes == 0);
}

}  // namespace

int main() {
    testServerBindEchoAndGracefulStop();
    testCallbackReentryForceEscalationPreservesFirstReason();
    testForeignObservationAndMutationRejectBeforeOwnerStateRead();
    testConnectionLimitSettlesProvisionalAdapter();
    testBindFailureIsTypedAndStillStopsPhysically();
    return 0;
}
