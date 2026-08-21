#pragma once

#include "gamenet/core/net/SocketTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::experimental::io_uring {

enum class IoUringOperationKind : std::uint8_t {
    Accept,
    Receive,
    Send,
};

enum class IoUringCompletionStatus : std::uint8_t {
    Succeeded,
    Failed,
    Cancelled,
};

enum class IoUringSubmissionResult : std::uint8_t {
    Accepted,
    SubmissionQueueFull,
    OperationLimit,
    PayloadTooLarge,
    ByteLimit,
    RejectedInvalid,
    RejectedQuiescing,
    RejectedShutdown,
};

enum class IoUringCancelResult : std::uint8_t {
    Accepted,
    AlreadyRequested,
    SubmissionQueueFull,
    RejectedInvalid,
    RejectedNotSubmitted,
    RejectedShutdown,
};

enum class IoUringPhase : std::uint8_t {
    Running,
    Quiescing,
    Shutdown,
};

enum class IoUringShutdownResult : std::uint8_t {
    Drained,
    TimedOut,
};

struct IoUringOperationIdentity {
    std::uint32_t slot{};
    std::uint32_t generation{};

    bool valid() const noexcept {
        return generation != 0;
    }

    bool operator==(const IoUringOperationIdentity&) const = default;
};

struct IoUringSubmissionOutcome {
    IoUringSubmissionResult result{IoUringSubmissionResult::RejectedInvalid};
    IoUringOperationIdentity identity{};
};

struct IoUringFlushOutcome {
    std::size_t submitted{};
    std::size_t pending{};
    int nativeError{};
};

struct IoUringWaitProgress {
    std::size_t completionQueueEntries{};
    std::size_t terminalNotices{};
    std::size_t internalCancelCqes{};
    std::size_t staleCqes{};
    bool budgetExhausted{};
    bool timedOut{};
};

struct IoUringCompletionEngineOptions {
    std::size_t entries{64};
    std::size_t maxOperations{64};
    std::size_t maxCompletionsPerWait{64};
    std::size_t maxBytesPerOperation{64U * 1024U};
    std::size_t maxOwnedBytes{4U * 1024U * 1024U};
};

struct IoUringCompletionEngineMetrics {
    std::size_t ringEntries{};
    std::uint64_t operationsAccepted{};
    std::uint64_t sqFullRejections{};
    std::uint64_t operationLimitRejections{};
    std::uint64_t payloadRejections{};
    std::uint64_t byteLimitRejections{};
    std::uint64_t cancelRequests{};
    std::uint64_t cancelCqes{};
    std::uint64_t terminalNotices{};
    std::uint64_t cancelledOperations{};
    std::uint64_t staleCqes{};
    std::uint64_t shutdownTimeouts{};
    std::uint64_t crossDomainFallbacks{};
    std::size_t activeOperations{};
    std::size_t pendingSubmissions{};
    std::size_t pendingCancelCompletions{};
    std::size_t readyNotices{};
    std::size_t ownedBytes{};
    std::size_t maxActiveOperations{};
    std::size_t maxReadyNotices{};
    std::size_t maxOwnedBytes{};
};

class IoUringCompletionEngineImpl;

class IoUringCompletionNotice {
public:
    IoUringCompletionNotice() noexcept;
    ~IoUringCompletionNotice();
    IoUringCompletionNotice(const IoUringCompletionNotice&) = delete;
    IoUringCompletionNotice& operator=(const IoUringCompletionNotice&) = delete;
    IoUringCompletionNotice(IoUringCompletionNotice&& other) noexcept;
    IoUringCompletionNotice& operator=(IoUringCompletionNotice&& other) noexcept;

    IoUringOperationIdentity identity() const noexcept;
    IoUringOperationKind kind() const noexcept;
    IoUringCompletionStatus status() const noexcept;
    std::size_t bytesTransferred() const noexcept;
    int nativeError() const noexcept;
    std::string_view payload() const noexcept;
    gamenet::net::SocketFd acceptedSocket() const noexcept;
    gamenet::net::SocketFd releaseAcceptedSocket() noexcept;

private:
    IoUringCompletionNotice(
        IoUringOperationIdentity identity,
        IoUringOperationKind kind,
        IoUringCompletionStatus status,
        std::size_t bytesTransferred,
        int nativeError,
        gamenet::net::SocketFd acceptedSocket,
        std::vector<std::byte> payload,
        std::shared_ptr<void> lease,
        std::size_t reservedBytes) noexcept;

    IoUringOperationIdentity identity_{};
    IoUringOperationKind kind_{IoUringOperationKind::Receive};
    IoUringCompletionStatus status_{IoUringCompletionStatus::Failed};
    std::size_t bytesTransferred_{};
    int nativeError_{};
    gamenet::net::SocketFd acceptedSocket_{gamenet::net::kInvalidSocket};
    std::vector<std::byte> payload_;
    std::shared_ptr<void> lease_;
    std::size_t reservedBytes_{};

    friend class IoUringCompletionEngineImpl;
};

class IoUringCompletionEngine {
public:
    explicit IoUringCompletionEngine(
        gamenet::net::EventLoop* ownerLoop,
        IoUringCompletionEngineOptions options = {});
    ~IoUringCompletionEngine();
    IoUringCompletionEngine(const IoUringCompletionEngine&) = delete;
    IoUringCompletionEngine& operator=(const IoUringCompletionEngine&) = delete;

    IoUringCompletionEngineOptions options() const noexcept;
    IoUringPhase phase() const noexcept;
    IoUringCompletionEngineMetrics metrics() const noexcept;
    gamenet::net::SocketFd completionDescriptor() const noexcept;

    IoUringSubmissionOutcome enqueueAccept(
        gamenet::net::SocketFd listenSocket,
        std::shared_ptr<void> lease = {});
    IoUringSubmissionOutcome enqueueRecv(
        gamenet::net::SocketFd socket,
        std::size_t maximumBytes,
        std::shared_ptr<void> lease = {});
    IoUringSubmissionOutcome enqueueSend(
        gamenet::net::SocketFd socket,
        std::string_view payload,
        std::shared_ptr<void> lease = {});
    IoUringFlushOutcome flush();
    IoUringCancelResult cancel(IoUringOperationIdentity identity);
    IoUringWaitProgress wait(std::chrono::milliseconds timeout);
    std::optional<IoUringCompletionNotice> takeNextNotice();

    void beginQuiesce();
    bool quiescent() const noexcept;
    IoUringShutdownResult shutdown(std::chrono::milliseconds timeout);

private:
    std::unique_ptr<IoUringCompletionEngineImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
