#include "IoUringEventLoopPump.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"

#include "core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::io_uring {

namespace {

IoUringEventLoopPumpOptions validatedOptions(
    gamenet::net::EventLoop* ownerLoop,
    IoUringEventLoopPumpOptions options,
    const IoUringEventLoopPump::CompletionConsumer& consumer) {
    if (ownerLoop == nullptr || options.maxNoticesPerTurn == 0 || !consumer) {
        throw std::invalid_argument(
            "IoUringEventLoopPump requires an owner, consumer, and finite dispatch budget");
    }
    return options;
}

}  // namespace

class IoUringEventLoopPumpImpl final
    : public std::enable_shared_from_this<IoUringEventLoopPumpImpl> {
public:
    enum class Phase : std::uint8_t {
        Running,
        Quiescing,
        Stopped,
    };

    IoUringEventLoopPumpImpl(
        gamenet::net::EventLoop* ownerLoop,
        IoUringEventLoopPumpOptions options,
        IoUringEventLoopPump::CompletionConsumer consumer,
        IoUringEventLoopPump::StoppedConsumer stoppedConsumer)
        : ownerLoop_(ownerLoop),
          options_(validatedOptions(ownerLoop, options, consumer)),
          consumer_(std::move(consumer)),
          stoppedConsumer_(std::move(stoppedConsumer)),
          engine_(ownerLoop, options_.engine),
          stopFuture_(stopPromise_.get_future().share()) {
        ownerLoop_->assertInLoopThread();
    }

    void initialize() {
        ownerLoop_->assertInLoopThread();
        const std::weak_ptr<IoUringEventLoopPumpImpl> weak = shared_from_this();
        try {
            channel_ = std::make_unique<gamenet::net::Channel>(
                ownerLoop_,
                engine_.completionDescriptor());
            channel_->setReadCallback([weak](gamenet::base::Timestamp) {
                if (const auto state = weak.lock()) {
                    state->driveOneTurn();
                }
            });
            channel_->setErrorCallback([weak] {
                if (const auto state = weak.lock()) {
                    state->driveOneTurn();
                }
            });
            channel_->enableReading();
            channelRegistered_ = true;
            lifecycleSource_ =
                gamenet::net::detail::EventLoopLifecycleRegistry::
                    attachQuiesceParticipant(*ownerLoop_, [weak] {
                        if (const auto state = weak.lock()) {
                            state->driveFromLifecycle();
                        }
                    });
            lifecycleAttached_ = true;
        } catch (...) {
            rollbackInitialization();
            throw;
        }
    }

    void destroy() noexcept {
        if (destroyed_) return;
        if (!ownerLoop_->isInLoopThread() || phase_ != Phase::Stopped ||
            stopFuture_.wait_for(std::chrono::milliseconds::zero()) !=
                std::future_status::ready ||
            channelRegistered_ || lifecycleAttached_ || !engine_.quiescent() ||
            engine_.metrics().readyNotices != 0) {
            std::terminate();
        }
        destroyed_ = true;
    }

    IoUringSubmissionOutcome enqueueAccept(
        gamenet::net::SocketFd listenSocket,
        std::shared_ptr<void> lease) {
        ownerLoop_->assertInLoopThread();
        synchronizeLoopPhase();
        auto outcome = engine_.enqueueAccept(listenSocket, std::move(lease));
        flushAccepted(outcome);
        return outcome;
    }

    IoUringSubmissionOutcome enqueueRecv(
        gamenet::net::SocketFd socket,
        std::size_t maximumBytes,
        std::shared_ptr<void> lease) {
        ownerLoop_->assertInLoopThread();
        synchronizeLoopPhase();
        auto outcome = engine_.enqueueRecv(socket, maximumBytes, std::move(lease));
        flushAccepted(outcome);
        return outcome;
    }

    IoUringSubmissionOutcome enqueueSend(
        gamenet::net::SocketFd socket,
        std::string_view payload,
        std::shared_ptr<void> lease) {
        ownerLoop_->assertInLoopThread();
        synchronizeLoopPhase();
        auto outcome = engine_.enqueueSend(socket, payload, std::move(lease));
        flushAccepted(outcome);
        return outcome;
    }

    IoUringCancelResult cancel(IoUringOperationIdentity identity) {
        ownerLoop_->assertInLoopThread();
        synchronizeLoopPhase();
        const auto result = engine_.cancel(identity);
        if (result == IoUringCancelResult::Accepted) {
            flushOrFailClosed();
        }
        return result;
    }

    void beginQuiesce() {
        ownerLoop_->assertInLoopThread();
        beginQuiesceInternal();
        requestContinuation();
    }

    bool stopped() const noexcept {
        return phase_ == Phase::Stopped;
    }

    IoUringEventLoopPumpMetrics metrics() const noexcept {
        return metrics_;
    }

    std::shared_future<IoUringEventLoopPumpStopSummary> stopFuture() const {
        return stopFuture_;
    }

private:
    void rollbackInitialization() noexcept {
        bool rollbackFailed = false;
        try {
            removeSources();
        } catch (...) {
            rollbackFailed = true;
        }
        try {
            rollbackFailed =
                engine_.shutdown(std::chrono::milliseconds::zero()) !=
                    IoUringShutdownResult::Drained ||
                !engine_.quiescent() || engine_.metrics().readyNotices != 0 ||
                rollbackFailed;
        } catch (...) {
            rollbackFailed = true;
        }
        if (rollbackFailed) std::terminate();
    }

    void synchronizeLoopPhase() {
        if (ownerLoop_->phase() != gamenet::net::EventLoopPhase::Running) {
            beginQuiesceInternal();
        }
    }

    void flushAccepted(const IoUringSubmissionOutcome& outcome) {
        if (outcome.result == IoUringSubmissionResult::Accepted) {
            flushOrFailClosed();
        }
    }

    void flushOrFailClosed() {
        try {
            const auto flushed = engine_.flush();
            if (flushed.nativeError != 0) {
                recordDriveFailure();
            }
        } catch (...) {
            recordDriveFailure();
        }
    }

    void recordDriveFailure() noexcept {
        ++metrics_.driveFailures;
        beginQuiesceInternal();
        requestContinuation();
    }

    void beginQuiesceInternal() noexcept {
        if (phase_ != Phase::Running) return;
        phase_ = Phase::Quiescing;
        ++metrics_.quiesceActivations;
        try {
            engine_.beginQuiesce();
        } catch (...) {
            ++metrics_.driveFailures;
        }
    }

    void driveFromLifecycle() {
        ownerLoop_->assertInLoopThread();
        synchronizeLoopPhase();
        driveOneTurn();
    }

    void driveOneTurn() {
        ownerLoop_->assertInLoopThread();
        if (phase_ == Phase::Stopped) return;
        if (driving_) {
            continuationRequested_ = true;
            return;
        }

        driving_ = true;
        ++metrics_.turns;
        synchronizeLoopPhase();
        try {
            (void)engine_.wait(std::chrono::milliseconds::zero());
        } catch (...) {
            recordDriveFailure();
        }

        std::size_t dispatched = 0;
        while (dispatched < options_.maxNoticesPerTurn) {
            auto notice = engine_.takeNextNotice();
            if (!notice) break;
            dispatchNotice(*notice);
            ++dispatched;
            synchronizeLoopPhase();
        }
        driving_ = false;

        const auto engineMetrics = engine_.metrics();
        const bool hasDecodedRemainder = engineMetrics.readyNotices != 0;
        const bool finalDrainPending =
            phase_ == Phase::Quiescing && !engine_.quiescent();
        if (continuationRequested_ || hasDecodedRemainder || finalDrainPending) {
            continuationRequested_ = false;
            requestContinuation();
        }
        tryFinishStopped();
    }

    void dispatchNotice(IoUringCompletionNotice& notice) noexcept {
        ++consumerDepth_;
        metrics_.maxConsumerDepth =
            (std::max)(metrics_.maxConsumerDepth, consumerDepth_);
        ++metrics_.noticesDispatched;
        try {
            consumer_(notice);
        } catch (...) {
            ++metrics_.consumerFailures;
        }
        --consumerDepth_;
    }

    void requestContinuation() noexcept {
        if (phase_ == Phase::Stopped) return;
        const auto result = lifecycleSource_.signal();
        if (result == gamenet::net::PostResult::Accepted) {
            ++metrics_.continuationSignals;
        } else if (ownerLoop_->phase() == gamenet::net::EventLoopPhase::Running) {
            ++metrics_.driveFailures;
        }
    }

    void tryFinishStopped() noexcept {
        if (phase_ != Phase::Quiescing || consumerDepth_ != 0 ||
            !engine_.quiescent() || engine_.metrics().readyNotices != 0) {
            return;
        }
        try {
            if (engine_.shutdown(std::chrono::milliseconds::zero()) !=
                IoUringShutdownResult::Drained) {
                ++metrics_.driveFailures;
                requestContinuation();
                return;
            }
            finishStopped();
        } catch (...) {
            recordDriveFailure();
        }
    }

    void finishStopped() {
        if (phase_ == Phase::Stopped) return;
        removeSources();
        phase_ = Phase::Stopped;
        auto makeSummary = [this] {
            return IoUringEventLoopPumpStopSummary{
                .result = (metrics_.driveFailures == 0 &&
                           metrics_.consumerFailures == 0)
                    ? IoUringEventLoopPumpStopResult::Drained
                    : IoUringEventLoopPumpStopResult::DrainedAfterFailure,
                .pump = metrics_,
                .engine = engine_.metrics(),
            };
        };
        auto summary = makeSummary();
        if (stoppedConsumer_) {
            try {
                stoppedConsumer_(summary);
            } catch (...) {
                ++metrics_.consumerFailures;
                summary = makeSummary();
            }
        }
        stopPromise_.set_value(std::move(summary));
    }

    void removeSources() {
        if (channelRegistered_) {
            channel_->disableAll();
            channel_->remove();
            channelRegistered_ = false;
        }
        if (lifecycleAttached_) {
            gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                *ownerLoop_, lifecycleSource_);
            lifecycleAttached_ = false;
        }
    }

    gamenet::net::EventLoop* ownerLoop_;
    IoUringEventLoopPumpOptions options_;
    IoUringEventLoopPump::CompletionConsumer consumer_;
    IoUringEventLoopPump::StoppedConsumer stoppedConsumer_;
    IoUringCompletionEngine engine_;
    std::unique_ptr<gamenet::net::Channel> channel_;
    gamenet::net::EventLoopLifecycleSource lifecycleSource_;
    std::promise<IoUringEventLoopPumpStopSummary> stopPromise_;
    std::shared_future<IoUringEventLoopPumpStopSummary> stopFuture_;
    IoUringEventLoopPumpMetrics metrics_{};
    Phase phase_{Phase::Running};
    std::size_t consumerDepth_{};
    bool lifecycleAttached_{false};
    bool channelRegistered_{false};
    bool driving_{false};
    bool continuationRequested_{false};
    bool destroyed_{false};
};

IoUringEventLoopPump::IoUringEventLoopPump(
    gamenet::net::EventLoop* ownerLoop,
    IoUringEventLoopPumpOptions options,
    CompletionConsumer consumer,
    StoppedConsumer stoppedConsumer)
    : impl_(std::make_shared<IoUringEventLoopPumpImpl>(
          ownerLoop,
          options,
          std::move(consumer),
          std::move(stoppedConsumer))) {
    impl_->initialize();
}

IoUringEventLoopPump::~IoUringEventLoopPump() {
    impl_->destroy();
}

IoUringSubmissionOutcome IoUringEventLoopPump::enqueueAccept(
    gamenet::net::SocketFd listenSocket,
    std::shared_ptr<void> lease) {
    return impl_->enqueueAccept(listenSocket, std::move(lease));
}

IoUringSubmissionOutcome IoUringEventLoopPump::enqueueRecv(
    gamenet::net::SocketFd socket,
    std::size_t maximumBytes,
    std::shared_ptr<void> lease) {
    return impl_->enqueueRecv(socket, maximumBytes, std::move(lease));
}

IoUringSubmissionOutcome IoUringEventLoopPump::enqueueSend(
    gamenet::net::SocketFd socket,
    std::string_view payload,
    std::shared_ptr<void> lease) {
    return impl_->enqueueSend(socket, payload, std::move(lease));
}

IoUringCancelResult IoUringEventLoopPump::cancel(
    IoUringOperationIdentity identity) {
    return impl_->cancel(identity);
}

void IoUringEventLoopPump::beginQuiesce() {
    impl_->beginQuiesce();
}

bool IoUringEventLoopPump::stopped() const noexcept {
    return impl_->stopped();
}

IoUringEventLoopPumpMetrics IoUringEventLoopPump::metrics() const noexcept {
    return impl_->metrics();
}

std::shared_future<IoUringEventLoopPumpStopSummary>
IoUringEventLoopPump::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
