// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IoUringEventLoopPump.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string_view>

namespace gamenet::net {
class InetAddress;
class Socket;
}

namespace gamenet::experimental::io_uring {

struct IoUringTcpConnectionIdentity {
    std::uint32_t slot{};
    std::uint32_t generation{};

    bool valid() const noexcept { return generation != 0; }
    bool operator==(const IoUringTcpConnectionIdentity&) const = default;
};

enum class IoUringTcpHubPhase : std::uint8_t {
    Running,
    Quiescing,
    Stopped,
};

enum class IoUringTcpHubAddResult : std::uint8_t {
    Accepted,
    ConnectionLimit,
    RejectedInvalid,
    EngineRejected,
    RejectedQuiescing,
    RejectedShutdown,
};

enum class IoUringTcpHubSendResult : std::uint8_t {
    Accepted,
    EmptyPayload,
    ConnectionByteLimit,
    ConnectionSegmentLimit,
    HubByteLimit,
    StaleConnection,
    Closing,
    EngineRejected,
};

enum class IoUringTcpHubReadControlResult : std::uint8_t {
    Applied,
    Unchanged,
    StaleConnection,
    Closing,
    EngineRejected,
};

enum class IoUringTcpHubCloseReason : std::uint8_t {
    Explicit,
    GracefulShutdown,
    PeerClosed,
    ReceiveFailed,
    SendFailed,
    EngineRejected,
    CallbackFailed,
    EventLoopQuiescing,
    HubStopped,
    Destroyed,
};

enum class IoUringTcpHubListenResult : std::uint8_t {
    Accepted,
    AlreadyListening,
    RejectedInvalid,
    EngineRejected,
    RejectedQuiescing,
    RejectedShutdown,
};

enum class IoUringTcpHubListenerCloseReason : std::uint8_t {
    Explicit,
    AcceptFailed,
    EngineRejected,
    CallbackFailed,
    EventLoopQuiescing,
    HubStopped,
    Destroyed,
};

struct IoUringTcpConnectionHubOptions {
    IoUringEventLoopPumpOptions pump{};
    std::size_t maxConnections{32};
    std::size_t maxTotalPendingSendBytes{4U * 1024U * 1024U};
    std::size_t maxReceiveBytes{4U * 1024U};
    std::size_t maxSendBytesPerOperation{4U * 1024U};
    std::size_t maxPendingSendBytesPerConnection{64U * 1024U};
    std::size_t maxPendingSendSegmentsPerConnection{64};
    std::size_t maxPendingAccepts{1};
};

enum class IoUringTcpHubConnectResult : std::uint8_t {
    Accepted,
    AlreadyConnecting,
    RejectedInvalid,
    EngineRejected,
    RejectedQuiescing,
    RejectedShutdown,
};

enum class IoUringTcpHubConnectCloseReason : std::uint8_t {
    Connected,
    Explicit,
    Timeout,
    Replaced,
    ConnectFailed,
    EngineRejected,
    CallbackFailed,
    EventLoopQuiescing,
    HubStopped,
    Destroyed,
};

// Typed result returned by an IOE-X12 accepted-socket ownership consumer.
// Accepted means the consumer retained or transferred the Socket RAII owner;
// every other result destroys it before the listener rearms.
enum class IoUringTcpHubAcceptedSocketResult : std::uint8_t {
    Accepted,
    QueueFull,
    WorkerShutdown,
    RejectedInvalid,
};

struct IoUringTcpConnectionHubAddOutcome;

struct IoUringTcpHubAcceptedConnectionCallbacks {
    std::function<void(IoUringTcpConnectionIdentity, std::string_view)>
        messageConsumer;
    std::function<void(
        IoUringTcpConnectionIdentity,
        IoUringTcpHubCloseReason)> closeConsumer;
    std::function<void(IoUringTcpConnectionIdentity, std::size_t)>
        outputProgressConsumer;
    // Called exactly once after the factory result is either admitted as one
    // Hub route or rejected. It transfers no socket ownership.
    std::function<void(const IoUringTcpConnectionHubAddOutcome&)>
        settlementConsumer;
};

struct IoUringTcpHubListenerMetrics {
    std::uint64_t acceptSubmissions{};
    std::uint64_t acceptTerminals{};
    std::uint64_t acceptCancellations{};
    std::uint64_t acceptCancellationRequests{};
    std::uint64_t acceptedSockets{};
    std::uint64_t connectionsAdmitted{};
    std::uint64_t acceptedSocketRejections{};
    std::uint64_t connectionLimitRejections{};
    std::uint64_t acceptedSocketHandoffs{};
    std::uint64_t handoffQueueFullRejections{};
    std::uint64_t handoffShutdownRejections{};
    std::uint64_t transientAcceptFailures{};
    std::uint64_t engineRejections{};
    std::uint64_t callbackFailures{};
    std::uint64_t stopRequests{};
    std::uint64_t listenerSocketCloseCount{};
    std::size_t activeAccepts{};
    std::size_t maxActiveAccepts{};
};

struct IoUringTcpHubListenerStopSummary {
    IoUringTcpHubListenerCloseReason reason{
        IoUringTcpHubListenerCloseReason::Explicit};
    int nativeError{};
    IoUringTcpHubListenerMetrics listener{};
    bool socketClosed{};
    bool acceptsRetired{};
};

struct IoUringTcpHubListenOutcome {
    IoUringTcpHubListenResult result{
        IoUringTcpHubListenResult::RejectedInvalid};
    std::shared_future<IoUringTcpHubListenerStopSummary> stopFuture{};
};

struct IoUringTcpHubConnectMetrics {
    std::uint64_t submissions{};
    std::uint64_t terminals{};
    std::uint64_t cancellationRequests{};
    std::uint64_t cancellations{};
    std::uint64_t successes{};
    std::uint64_t failures{};
    std::uint64_t callbackFailures{};
    std::uint64_t socketCloseCount{};
    std::uint64_t socketTransferCount{};
    std::size_t activeAttempts{};
    std::size_t maxActiveAttempts{};
};

struct IoUringTcpHubConnectStopSummary {
    IoUringTcpHubConnectCloseReason reason{
        IoUringTcpHubConnectCloseReason::Explicit};
    int nativeError{};
    IoUringOperationIdentity operation{};
    IoUringTcpHubConnectMetrics connect{};
    IoUringTcpHubAddResult connectionResult{
        IoUringTcpHubAddResult::RejectedInvalid};
    IoUringTcpConnectionIdentity connectionIdentity{};
    bool operationRetired{};
    bool socketClosed{};
    bool socketTransferredToConnection{};
};

struct IoUringTcpHubConnectOutcome {
    IoUringTcpHubConnectResult result{
        IoUringTcpHubConnectResult::RejectedInvalid};
    IoUringOperationIdentity operation{};
    std::shared_future<IoUringTcpHubConnectStopSummary> stopFuture{};
};

struct IoUringTcpConnectionHubConnectionMetrics {
    std::uint64_t receiveSubmissions{};
    std::uint64_t receiveTerminals{};
    std::uint64_t receiveCancelRequests{};
    std::uint64_t receiveCancellations{};
    std::uint64_t messagesDelivered{};
    std::uint64_t bytesReceived{};
    std::uint64_t sendAdmissions{};
    std::uint64_t sendSubmissions{};
    std::uint64_t sendTerminals{};
    std::uint64_t bytesSent{};
    std::uint64_t bytesDiscarded{};
    std::uint64_t closeRequests{};
    std::uint64_t gracefulShutdownRequests{};
    std::uint64_t writeHalfCloses{};
    std::uint64_t callbackFailures{};
    std::size_t pendingSendBytes{};
    std::size_t pendingSendSegments{};
    std::size_t maxPendingSendBytes{};
    std::size_t maxPendingSendSegments{};
    std::size_t maxActiveReceives{};
    std::size_t maxActiveSends{};
};

struct IoUringTcpConnectionHubMetrics {
    std::uint64_t connectionsAccepted{};
    std::uint64_t connectionsRetired{};
    std::uint64_t messagesDelivered{};
    std::uint64_t bytesReceived{};
    std::uint64_t sendAdmissions{};
    std::uint64_t bytesSent{};
    std::uint64_t bytesDiscarded{};
    std::uint64_t gracefulShutdownRequests{};
    std::uint64_t writeHalfCloses{};
    std::uint64_t connectionByteLimitRejections{};
    std::uint64_t connectionSegmentLimitRejections{};
    std::uint64_t hubByteLimitRejections{};
    std::uint64_t staleConnectionRejections{};
    std::uint64_t engineRejections{};
    std::uint64_t callbackFailures{};
    std::uint64_t invariantFailures{};
    std::uint64_t foreignMutationRejections{};
    std::uint64_t socketCloseCount{};
    std::size_t activeConnections{};
    std::size_t maxActiveConnections{};
    std::size_t activeOperationRoutes{};
    std::size_t maxActiveOperationRoutes{};
    std::size_t maxActiveReceives{};
    std::size_t maxActiveSends{};
    std::size_t pendingSendBytes{};
    std::size_t maxPendingSendBytes{};
    std::size_t maxConsumerDepth{};
};

struct IoUringTcpConnectionHubConnectionStopSummary {
    IoUringTcpConnectionIdentity identity{};
    IoUringTcpHubCloseReason reason{IoUringTcpHubCloseReason::Explicit};
    int nativeError{};
    IoUringTcpConnectionHubConnectionMetrics connection{};
    bool socketClosed{};
};

struct IoUringTcpConnectionHubStopSummary {
    IoUringTcpConnectionHubMetrics hub{};
    IoUringEventLoopPumpStopSummary pump{};
    std::optional<IoUringTcpHubListenerStopSummary> listener;
    std::optional<IoUringTcpHubConnectStopSummary> connect;
    bool allConnectionsStopped{};
};

struct IoUringTcpConnectionHubAddOutcome {
    IoUringTcpHubAddResult result{IoUringTcpHubAddResult::RejectedInvalid};
    IoUringTcpConnectionIdentity identity{};
    std::shared_future<IoUringTcpConnectionHubConnectionStopSummary> stopFuture{};
};

class IoUringTcpConnectionHubImpl;

// Linux-only experimental transport used by the installed Server/Client
// façades. Multiple established stream
// connections can share one EventLoop/Pump/Engine without per-connection
// rings, Channels, or worker threads.
class IoUringTcpConnectionHub {
public:
    using MessageConsumer = std::function<void(
        IoUringTcpConnectionIdentity,
        std::string_view)>;
    using CloseConsumer = std::function<void(
        IoUringTcpConnectionIdentity,
        IoUringTcpHubCloseReason)>;
    using OutputProgressConsumer = std::function<void(
        IoUringTcpConnectionIdentity,
        std::size_t)>;
    using StoppedConsumer =
        std::function<void(const IoUringTcpConnectionHubStopSummary&)>;
    using AcceptedConnectionFactory =
        std::function<IoUringTcpHubAcceptedConnectionCallbacks()>;
    using AcceptedSocketConsumer = std::function<
        IoUringTcpHubAcceptedSocketResult(
            std::unique_ptr<gamenet::net::Socket>,
            const gamenet::net::InetAddress&)>;
    using ListenerStoppedConsumer =
        std::function<void(const IoUringTcpHubListenerStopSummary&)>;
    using ConnectStoppedConsumer =
        std::function<void(const IoUringTcpHubConnectStopSummary&)>;

    IoUringTcpConnectionHub(
        gamenet::net::EventLoop* ownerLoop,
        IoUringTcpConnectionHubOptions options,
        StoppedConsumer stoppedConsumer = {});
    ~IoUringTcpConnectionHub();
    IoUringTcpConnectionHub(const IoUringTcpConnectionHub&) = delete;
    IoUringTcpConnectionHub& operator=(const IoUringTcpConnectionHub&) = delete;

    // Socket ownership transfers on every call, including rejected outcomes.
    IoUringTcpConnectionHubAddOutcome addConnection(
        gamenet::net::SocketFd establishedSocket,
        MessageConsumer messageConsumer,
        CloseConsumer closeConsumer = {},
        OutputProgressConsumer outputProgressConsumer = {});
    // The already-bound/listening socket transfers on every call. The factory
    // returns callbacks only; accepted descriptors never escape Hub ownership.
    IoUringTcpHubListenOutcome listen(
        gamenet::net::SocketFd listeningSocket,
        AcceptedConnectionFactory connectionFactory,
        ListenerStoppedConsumer stoppedConsumer = {});
    // IOE-X12 listener-only mode. The accepted Socket is uniquely owned by the
    // consumer call. Returning Accepted requires the consumer to retain or
    // transfer that RAII owner; rejection and unwinding close it exactly once.
    IoUringTcpHubListenOutcome listenAndHandoff(
        gamenet::net::SocketFd listeningSocket,
        AcceptedSocketConsumer socketConsumer,
        ListenerStoppedConsumer stoppedConsumer = {});
    // IOE-X13 one-shot active connect. Socket ownership transfers on every
    // result. Success moves it directly into one normal Hub Route created from
    // connectionFactory; all other terminals close it after operation retire.
    IoUringTcpHubConnectOutcome connect(
        gamenet::net::SocketFd socket,
        const gamenet::net::InetAddress& peer,
        AcceptedConnectionFactory connectionFactory,
        ConnectStoppedConsumer stoppedConsumer = {});
    bool cancelConnect(
        IoUringOperationIdentity operation,
        IoUringTcpHubConnectCloseReason reason =
            IoUringTcpHubConnectCloseReason::Explicit);
    bool stopListening();
    // Mutable Hub observations are owner-loop-only and reject foreign reads.
    bool listening() const;
    IoUringTcpHubListenerMetrics listenerMetrics() const;
    IoUringTcpHubSendResult send(
        IoUringTcpConnectionIdentity connection,
        std::string_view payload);
    IoUringTcpHubReadControlResult pauseRead(
        IoUringTcpConnectionIdentity connection);
    IoUringTcpHubReadControlResult resumeRead(
        IoUringTcpConnectionIdentity connection);
    bool shutdownConnection(
        IoUringTcpConnectionIdentity connection);
    bool closeConnection(
        IoUringTcpConnectionIdentity connection,
        IoUringTcpHubCloseReason reason = IoUringTcpHubCloseReason::Explicit);
    bool stop();

    IoUringTcpHubPhase phase() const;
    IoUringTcpConnectionHubMetrics metrics() const;
    std::shared_future<IoUringTcpConnectionHubStopSummary> stopFuture() const;

private:
    std::unique_ptr<IoUringTcpConnectionHubImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
