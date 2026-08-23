// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// IOE-X12 source-private composition: one accept owner hands accepted sockets
// to finitely many worker-owned Hub/Pump/Engine instances.

#include "IoUringTcpConnectionAdapter.h"
#include "IoUringTcpServer.h"

#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/EventLoopThreadPool.h"
#include "gamenet/core/net/InetAddress.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>
#include <vector>

namespace gamenet::experimental::io_uring {

enum class IoUringTcpMultiOwnerServerPhase : std::uint8_t {
    Configuring,
    Starting,
    Running,
    Quiescing,
    Stopped,
};

struct IoUringTcpMultiOwnerServerOptions {
    IoUringTcpConnectionHubOptions acceptHub{};
    IoUringTcpConnectionHubOptions workerHub{};
    IoUringTcpConnectionAdapterOptions connection{};
    std::size_t workerCount{2};
    std::size_t maxPendingHandoffs{256};
    gamenet::net::EventLoopSelectionPolicy placementPolicy{
        gamenet::net::EventLoopSelectionPolicy::RoundRobin};
    bool reuseAddress{true};
    bool reusePort{true};
};

struct IoUringTcpMultiOwnerServerMetrics {
    std::uint64_t startAttempts{};
    std::uint64_t socketCreateFailures{};
    std::uint64_t bindFailures{};
    std::uint64_t listenFailures{};
    std::uint64_t hubAdmissionFailures{};
    std::uint64_t handoffsAccepted{};
    std::uint64_t handoffLimitRejections{};
    std::uint64_t handoffQueueRejections{};
    std::uint64_t handoffShutdownRejections{};
    std::uint64_t workerAdmissionRejections{};
    std::uint64_t connectionsEstablished{};
    std::uint64_t connectionsRetired{};
    std::uint64_t callbackFailures{};
    std::uint64_t gracefulStopRequests{};
    std::uint64_t forceStopRequests{};
    std::size_t pendingHandoffs{};
    std::size_t maxPendingHandoffs{};
    std::size_t activeConnections{};
    std::size_t maxActiveConnections{};
    bool listenerStopped{};
    bool forceEscalated{};
};

struct IoUringTcpMultiOwnerWorkerStopSummary {
    std::size_t workerIndex{};
    std::uint64_t connectionsEstablished{};
    std::uint64_t connectionsRetired{};
    std::uint64_t admissionRejections{};
    std::uint64_t callbackFailures{};
    std::size_t activeConnections{};
    IoUringTcpConnectionHubStopSummary hub{};
    bool ownerDestroyedHub{};
};

struct IoUringTcpMultiOwnerServerStopSummary {
    IoUringTcpMultiOwnerServerMetrics server{};
    IoUringTcpConnectionHubStopSummary acceptHub{};
    std::vector<IoUringTcpMultiOwnerWorkerStopSummary> workers;
    bool listenerStoppedBeforeWorkers{};
    bool allHandoffsSettled{};
    bool allWorkersStopped{};
};

class IoUringTcpMultiOwnerServerImpl;

class IoUringTcpMultiOwnerServer {
public:
    using ConnectionCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t)>;
    using MessageCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t,
        std::string_view)>;
    using HighWaterMarkCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t,
        std::size_t)>;
    using WriteCompleteCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t)>;
    using CloseInfoCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t,
        const gamenet::net::TcpConnectionCloseInfo&)>;
    using StoppedConsumer = std::function<void(
        const IoUringTcpMultiOwnerServerStopSummary&)>;

    IoUringTcpMultiOwnerServer(
        gamenet::net::EventLoop* acceptLoop,
        gamenet::net::InetAddress listenAddress,
        IoUringTcpMultiOwnerServerOptions options = {},
        StoppedConsumer stoppedConsumer = {});
    ~IoUringTcpMultiOwnerServer();
    IoUringTcpMultiOwnerServer(
        const IoUringTcpMultiOwnerServer&) = delete;
    IoUringTcpMultiOwnerServer& operator=(
        const IoUringTcpMultiOwnerServer&) = delete;

    void setWorkerInitCallback(gamenet::net::ThreadInitCallback callback);
    void setConnectionCallback(ConnectionCallback callback);
    void setMessageCallback(MessageCallback callback);
    void setHighWaterMarkCallback(HighWaterMarkCallback callback);
    void setWriteCompleteCallback(WriteCompleteCallback callback);
    void setCloseInfoCallback(CloseInfoCallback callback);

    IoUringTcpServerStartOutcome start();
    std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
    stopGracefully();
    std::shared_future<IoUringTcpMultiOwnerServerStopSummary> forceStop();

    IoUringTcpMultiOwnerServerPhase phase() const;
    gamenet::net::InetAddress listenAddress() const;
    IoUringTcpMultiOwnerServerMetrics metrics() const;
    std::vector<gamenet::net::EventLoopExecutor> workerExecutors() const;
    std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
    stopFuture() const;

private:
    std::unique_ptr<IoUringTcpMultiOwnerServerImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
