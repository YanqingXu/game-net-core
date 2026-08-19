#pragma once

#include "IoUringEventLoopPump.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

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
    PeerClosed,
    ReceiveFailed,
    SendFailed,
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
    std::size_t pendingSendBytes{};
    std::size_t maxPendingSendBytes{};
    std::size_t maxConsumerDepth{};
};

struct IoUringTcpConnectionHubConnectionStopSummary {
    IoUringTcpConnectionIdentity identity{};
    IoUringTcpHubCloseReason reason{IoUringTcpHubCloseReason::Explicit};
    IoUringTcpConnectionHubConnectionMetrics connection{};
    bool socketClosed{};
};

struct IoUringTcpConnectionHubStopSummary {
    IoUringTcpConnectionHubMetrics hub{};
    IoUringEventLoopPumpStopSummary pump{};
    bool allConnectionsStopped{};
};

struct IoUringTcpConnectionHubAddOutcome {
    IoUringTcpHubAddResult result{IoUringTcpHubAddResult::RejectedInvalid};
    IoUringTcpConnectionIdentity identity{};
    std::shared_future<IoUringTcpConnectionHubConnectionStopSummary> stopFuture{};
};

class IoUringTcpConnectionHubImpl;

// Linux-only, source-private proof that multiple established stream
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
    using StoppedConsumer =
        std::function<void(const IoUringTcpConnectionHubStopSummary&)>;

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
        CloseConsumer closeConsumer = {});
    IoUringTcpHubSendResult send(
        IoUringTcpConnectionIdentity connection,
        std::string_view payload);
    IoUringTcpHubReadControlResult pauseRead(
        IoUringTcpConnectionIdentity connection);
    IoUringTcpHubReadControlResult resumeRead(
        IoUringTcpConnectionIdentity connection);
    bool closeConnection(
        IoUringTcpConnectionIdentity connection,
        IoUringTcpHubCloseReason reason = IoUringTcpHubCloseReason::Explicit);
    bool stop();

    IoUringTcpHubPhase phase() const noexcept;
    IoUringTcpConnectionHubMetrics metrics() const noexcept;
    std::shared_future<IoUringTcpConnectionHubStopSummary> stopFuture() const;

private:
    std::unique_ptr<IoUringTcpConnectionHubImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
