// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// IoUringTcpServer is the explicit Linux-only experimental façade over the
// IOE-X11 single-owner listener/Hub/Adapter composition. It does not change or
// select the production TcpServer backend.

#include "IoUringTcpConnectionAdapter.h"

#include "gamenet/core/net/InetAddress.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

namespace gamenet::experimental::io_uring {

enum class IoUringTcpServerPhase : std::uint8_t {
    Configuring,
    Starting,
    Running,
    Quiescing,
    Stopped,
};

enum class IoUringTcpServerStartResult : std::uint8_t {
    Accepted,
    AlreadyStarted,
    SocketCreateFailed,
    BindFailed,
    ListenFailed,
    HubRejected,
    EngineRejected,
    RejectedQuiescing,
    RejectedShutdown,
};

struct IoUringTcpServerOptions {
    IoUringTcpConnectionHubOptions hub{};
    IoUringTcpConnectionAdapterOptions connection{};
    bool reuseAddress{true};
    bool reusePort{true};
};

struct IoUringTcpServerStartOutcome {
    IoUringTcpServerStartResult result{
        IoUringTcpServerStartResult::HubRejected};
    int nativeError{};
    gamenet::net::InetAddress listenAddress{};
};

struct IoUringTcpServerMetrics {
    std::uint64_t startAttempts{};
    std::uint64_t socketCreateFailures{};
    std::uint64_t bindFailures{};
    std::uint64_t listenFailures{};
    std::uint64_t hubAdmissionFailures{};
    std::uint64_t connectionsEstablished{};
    std::uint64_t connectionsRetired{};
    std::uint64_t connectionAdmissionRejections{};
    std::uint64_t callbackFailures{};
    std::uint64_t gracefulStopRequests{};
    std::uint64_t forceStopRequests{};
    std::size_t activeConnections{};
    std::size_t maxActiveConnections{};
    std::size_t provisionalConnections{};
    bool listenerStopped{};
    bool forceEscalated{};
};

struct IoUringTcpServerStopSummary {
    IoUringTcpServerMetrics server{};
    IoUringTcpConnectionHubStopSummary hub{};
    bool listenerStoppedBeforeHub{};
    bool allAdaptersRetired{};
};

class IoUringTcpServerImpl;

class IoUringTcpServer {
public:
    using ConnectionCallback =
        std::function<void(IoUringTcpConnectionAdapter&)>;
    using MessageCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::string_view)>;
    using HighWaterMarkCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t)>;
    using WriteCompleteCallback =
        std::function<void(IoUringTcpConnectionAdapter&)>;
    using CloseInfoCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        const gamenet::net::TcpConnectionCloseInfo&)>;
    using StoppedConsumer =
        std::function<void(const IoUringTcpServerStopSummary&)>;

    IoUringTcpServer(
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::InetAddress listenAddress,
        IoUringTcpServerOptions options = {},
        StoppedConsumer stoppedConsumer = {});
    ~IoUringTcpServer();
    IoUringTcpServer(const IoUringTcpServer&) = delete;
    IoUringTcpServer& operator=(const IoUringTcpServer&) = delete;

    void setConnectionCallback(ConnectionCallback callback);
    void setMessageCallback(MessageCallback callback);
    void setHighWaterMarkCallback(HighWaterMarkCallback callback);
    void setWriteCompleteCallback(WriteCompleteCallback callback);
    void setCloseInfoCallback(CloseInfoCallback callback);

    IoUringTcpServerStartOutcome start();
    std::shared_future<IoUringTcpServerStopSummary> stopGracefully();
    std::shared_future<IoUringTcpServerStopSummary> forceStop();

    IoUringTcpServerPhase phase() const;
    gamenet::net::InetAddress listenAddress() const;
    std::size_t connectionCount() const;
    IoUringTcpServerMetrics metrics() const;
    std::shared_future<IoUringTcpServerStopSummary> stopFuture() const;

private:
    std::unique_ptr<IoUringTcpServerImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
