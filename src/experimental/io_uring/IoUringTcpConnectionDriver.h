#pragma once

#include "IoUringEventLoopPump.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

namespace gamenet::experimental::io_uring {

enum class IoUringTcpDriverPhase : std::uint8_t {
    Created,
    Running,
    Closing,
    Stopped,
};

enum class IoUringTcpDriverStartResult : std::uint8_t {
    Accepted,
    AlreadyStarted,
    EngineRejected,
    RejectedClosing,
};

enum class IoUringTcpDriverSendResult : std::uint8_t {
    Accepted,
    EmptyPayload,
    ByteLimit,
    SegmentLimit,
    EngineRejected,
    RejectedNotRunning,
    RejectedClosing,
};

enum class IoUringTcpDriverReadControlResult : std::uint8_t {
    Applied,
    Unchanged,
    RejectedNotRunning,
    RejectedClosing,
};

enum class IoUringTcpDriverCloseReason : std::uint8_t {
    Explicit,
    PeerClosed,
    ReceiveFailed,
    SendFailed,
    EngineRejected,
    CallbackFailed,
    EventLoopQuiescing,
    Destroyed,
};

struct IoUringTcpConnectionDriverOptions {
    IoUringEventLoopPumpOptions pump{};
    std::size_t maxReceiveBytes{4U * 1024U};
    std::size_t maxSendBytesPerOperation{4U * 1024U};
    std::size_t maxPendingSendBytes{64U * 1024U};
    std::size_t maxPendingSendSegments{64};
};

struct IoUringTcpConnectionDriverMetrics {
    std::uint64_t receiveSubmissions{};
    std::uint64_t receiveTerminals{};
    std::uint64_t receiveCancelRequests{};
    std::uint64_t receiveCancellations{};
    std::uint64_t messagesDelivered{};
    std::uint64_t bytesReceived{};
    std::uint64_t readPauses{};
    std::uint64_t readResumes{};
    std::size_t activeReceives{};
    std::size_t maxActiveReceives{};
    std::uint64_t sendAdmissions{};
    std::uint64_t sendSubmissions{};
    std::uint64_t sendTerminals{};
    std::uint64_t bytesSent{};
    std::uint64_t bytesDiscarded{};
    std::uint64_t byteLimitRejections{};
    std::uint64_t segmentLimitRejections{};
    std::uint64_t engineRejections{};
    std::size_t pendingSendBytes{};
    std::size_t pendingSendSegments{};
    std::size_t maxPendingSendBytes{};
    std::size_t maxPendingSendSegments{};
    std::size_t activeSends{};
    std::size_t maxActiveSends{};
    std::uint64_t closeRequests{};
    std::uint64_t duplicateCloseRequests{};
    std::uint64_t callbackFailures{};
    std::uint64_t invariantFailures{};
    std::uint64_t foreignMutationRejections{};
    std::uint64_t socketCloseCount{};
    std::size_t maxCallbackDepth{};
};

struct IoUringTcpConnectionDriverStopSummary {
    IoUringTcpDriverCloseReason reason{IoUringTcpDriverCloseReason::Explicit};
    IoUringTcpConnectionDriverMetrics driver{};
    IoUringEventLoopPumpStopSummary pump{};
    bool socketClosed{};
};

class IoUringTcpConnectionDriverImpl;

// Linux-only, source-private proof of one established stream connection over
// the IOE-X2 completion pump. It is deliberately not a production
// TcpConnection implementation or an installed backend-selection surface.
class IoUringTcpConnectionDriver {
public:
    using MessageConsumer = std::function<void(std::string_view)>;
    using CloseConsumer = std::function<void(IoUringTcpDriverCloseReason)>;

    IoUringTcpConnectionDriver(
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::SocketFd establishedSocket,
        IoUringTcpConnectionDriverOptions options,
        MessageConsumer messageConsumer,
        CloseConsumer closeConsumer = {});
    ~IoUringTcpConnectionDriver();
    IoUringTcpConnectionDriver(const IoUringTcpConnectionDriver&) = delete;
    IoUringTcpConnectionDriver& operator=(
        const IoUringTcpConnectionDriver&) = delete;

    IoUringTcpDriverStartResult start();
    IoUringTcpDriverSendResult send(std::string_view payload);
    IoUringTcpDriverReadControlResult pauseRead();
    IoUringTcpDriverReadControlResult resumeRead();
    bool close(
        IoUringTcpDriverCloseReason reason =
            IoUringTcpDriverCloseReason::Explicit);

    IoUringTcpDriverPhase phase() const noexcept;
    bool readPaused() const noexcept;
    IoUringTcpConnectionDriverMetrics metrics() const noexcept;
    std::shared_future<IoUringTcpConnectionDriverStopSummary> stopFuture() const;

private:
    std::unique_ptr<IoUringTcpConnectionDriverImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
