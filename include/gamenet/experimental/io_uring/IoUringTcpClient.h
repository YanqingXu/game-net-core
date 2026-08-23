// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// IOE-X13 owner-loop-only active TCP client exposed through the explicit
// Linux-only experimental package component.

#include "IoUringTcpConnectionAdapter.h"

#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/InetAddress.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string_view>

namespace gamenet::experimental::io_uring {

enum class IoUringTcpClientPhase : std::uint8_t {
    Idle,
    Connecting,
    RetryWaiting,
    Connected,
    Quiescing,
    Stopped,
};

enum class IoUringTcpClientConnectResult : std::uint8_t {
    Accepted,
    AlreadyActive,
    SocketCreateFailed,
    EngineRejected,
    RejectedQuiescing,
    RejectedShutdown,
};

struct IoUringTcpClientConnectOutcome {
    IoUringTcpClientConnectResult result{
        IoUringTcpClientConnectResult::EngineRejected};
    std::uint64_t generation{};
    int nativeError{};
};

struct IoUringTcpClientOptions {
    using Duration = std::chrono::steady_clock::duration;

    IoUringTcpConnectionHubOptions hub{};
    IoUringTcpConnectionAdapterOptions connection{};
    Duration initialRetryDelay{std::chrono::milliseconds(500)};
    Duration maximumRetryDelay{std::chrono::seconds(30)};
    // nullopt disables the user-space deadline. A present zero duration is an
    // immediate deterministic deadline and is useful for exact race contracts.
    std::optional<Duration> connectTimeout{};
    bool retryEnabled{false};

    void validate() const;
};

struct IoUringTcpClientMetrics {
    std::uint64_t connectRequests{};
    std::uint64_t restartRequests{};
    std::uint64_t disconnectRequests{};
    std::uint64_t stopRequests{};
    std::uint64_t attemptsStarted{};
    std::uint64_t connectSuccesses{};
    std::uint64_t connectFailures{};
    std::uint64_t connectTimeouts{};
    std::uint64_t connectCancellations{};
    std::uint64_t retriesScheduled{};
    std::uint64_t staleTerminals{};
    std::uint64_t socketCreateFailures{};
    std::uint64_t attemptSocketsCreated{};
    std::uint64_t attemptSocketsClosed{};
    std::uint64_t attemptSocketsTransferred{};
    std::uint64_t adapterAdmissionFailures{};
    std::uint64_t connectionsEstablished{};
    std::uint64_t connectionsRetired{};
    std::uint64_t callbackFailures{};
    std::size_t activeConnections{};
    std::size_t activeAttempts{};
    bool retryTimerPending{};
    bool timeoutTimerPending{};
    bool hubDestroyed{};
};

struct IoUringTcpClientStopSummary {
    IoUringTcpClientMetrics client{};
    IoUringTcpConnectionHubStopSummary hub{};
    bool allTimersRetired{};
    bool allAttemptsRetired{};
    bool allConnectionsStopped{};
    bool ownerDestroyedHub{};
};

class IoUringTcpClientImpl;

class IoUringTcpClient {
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
        std::function<void(const IoUringTcpClientStopSummary&)>;

    IoUringTcpClient(
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::InetAddress serverAddress,
        IoUringTcpClientOptions options = {},
        StoppedConsumer stoppedConsumer = {});
    ~IoUringTcpClient();
    IoUringTcpClient(const IoUringTcpClient&) = delete;
    IoUringTcpClient& operator=(const IoUringTcpClient&) = delete;

    void setConnectorEventCallback(
        gamenet::net::ConnectorEventCallback callback);
    void setConnectionCallback(ConnectionCallback callback);
    void setMessageCallback(MessageCallback callback);
    void setHighWaterMarkCallback(HighWaterMarkCallback callback);
    void setWriteCompleteCallback(WriteCompleteCallback callback);
    void setCloseInfoCallback(CloseInfoCallback callback);

    IoUringTcpClientConnectOutcome connect();
    IoUringTcpClientConnectOutcome restart();
    bool disconnect();
    std::shared_future<IoUringTcpClientStopSummary> stop();

    IoUringTcpClientPhase phase() const;
    bool connected() const;
    IoUringTcpConnectionAdapter* connection() const;
    IoUringTcpClientMetrics metrics() const;
    std::shared_future<IoUringTcpClientStopSummary> stopFuture() const;

private:
    std::unique_ptr<IoUringTcpClientImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
