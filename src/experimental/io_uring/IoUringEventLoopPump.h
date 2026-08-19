#pragma once

#include "IoUringCompletionEngine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

namespace gamenet::experimental::io_uring {

enum class IoUringEventLoopPumpStopResult : std::uint8_t {
    Drained,
    DrainedAfterFailure,
};

struct IoUringEventLoopPumpOptions {
    IoUringCompletionEngineOptions engine{};
    std::size_t maxNoticesPerTurn{16};
};

struct IoUringEventLoopPumpMetrics {
    std::uint64_t turns{};
    std::uint64_t noticesDispatched{};
    std::uint64_t continuationSignals{};
    std::uint64_t quiesceActivations{};
    std::uint64_t driveFailures{};
    std::uint64_t consumerFailures{};
    std::size_t maxConsumerDepth{};
};

struct IoUringEventLoopPumpStopSummary {
    IoUringEventLoopPumpStopResult result{
        IoUringEventLoopPumpStopResult::Drained};
    IoUringEventLoopPumpMetrics pump{};
    IoUringCompletionEngineMetrics engine{};
};

class IoUringEventLoopPumpImpl;

// Linux-only, source-private bridge from EventLoop scheduling to the
// experimental one-shot Completion Engine. The completion descriptor Channel
// is only a readiness trigger; operation identity/result/lease remain typed
// Engine state and are consumed on the EventLoop owner thread.
class IoUringEventLoopPump {
public:
    using CompletionConsumer =
        std::function<void(IoUringCompletionNotice&)>;

    IoUringEventLoopPump(
        gamenet::net::EventLoop* ownerLoop,
        IoUringEventLoopPumpOptions options,
        CompletionConsumer consumer);
    ~IoUringEventLoopPump();
    IoUringEventLoopPump(const IoUringEventLoopPump&) = delete;
    IoUringEventLoopPump& operator=(const IoUringEventLoopPump&) = delete;

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
    IoUringCancelResult cancel(IoUringOperationIdentity identity);

    void beginQuiesce();
    bool stopped() const noexcept;
    IoUringEventLoopPumpMetrics metrics() const noexcept;
    std::shared_future<IoUringEventLoopPumpStopSummary> stopFuture() const;

private:
    std::shared_ptr<IoUringEventLoopPumpImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
