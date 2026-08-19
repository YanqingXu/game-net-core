#include "IoUringTcpConnectionDriver.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

namespace gamenet::experimental::io_uring {

namespace {

IoUringTcpConnectionDriverOptions validateOptions(
    gamenet::net::EventLoop* ownerLoop,
    gamenet::net::SocketFd socket,
    IoUringTcpConnectionDriverOptions options,
    const IoUringTcpConnectionDriver::MessageConsumer& messageConsumer) {
    if (ownerLoop == nullptr || !gamenet::net::sockets::isValid(socket) ||
        !messageConsumer || options.maxReceiveBytes == 0 ||
        options.maxSendBytesPerOperation == 0 ||
        options.maxPendingSendBytes == 0 ||
        options.maxPendingSendSegments == 0 ||
        options.pump.engine.entries < 4 ||
        options.pump.engine.maxOperations < 2 ||
        options.maxReceiveBytes > options.pump.engine.maxBytesPerOperation ||
        options.maxSendBytesPerOperation >
            options.pump.engine.maxBytesPerOperation ||
        options.maxSendBytesPerOperation > options.maxPendingSendBytes ||
        options.maxReceiveBytes > options.pump.engine.maxOwnedBytes ||
        options.maxSendBytesPerOperation >
            options.pump.engine.maxOwnedBytes - options.maxReceiveBytes) {
        throw std::invalid_argument(
            "IoUringTcpConnectionDriver requires a transferred socket, "
            "consumer, and finite concurrent Recv/Send budgets");
    }
    return options;
}

bool sameIdentity(
    IoUringOperationIdentity left,
    IoUringOperationIdentity right) noexcept {
    return left == right;
}

}  // namespace

class IoUringTcpConnectionDriverImpl final {
public:
    IoUringTcpConnectionDriverImpl(
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::SocketFd establishedSocket,
        IoUringTcpConnectionDriverOptions options,
        IoUringTcpConnectionDriver::MessageConsumer messageConsumer,
        IoUringTcpConnectionDriver::CloseConsumer closeConsumer)
        : ownerLoop_(ownerLoop),
          socket_(establishedSocket),
          options_(validateOptions(
              ownerLoop,
              establishedSocket,
              options,
              messageConsumer)),
          messageConsumer_(std::move(messageConsumer)),
          closeConsumer_(std::move(closeConsumer)),
          stopFuture_(stopPromise_.get_future().share()) {
        ownerLoop_->assertInLoopThread();
        pump_ = std::make_unique<IoUringEventLoopPump>(
            ownerLoop_,
            options_.pump,
            [this](IoUringCompletionNotice& notice) { handleNotice(notice); },
            [this](const IoUringEventLoopPumpStopSummary& summary) {
                handlePumpStopped(summary);
            });
    }

    ~IoUringTcpConnectionDriverImpl() {
        if (!pump_) return;
        try {
            assertOwner();
            if (phase_ != IoUringTcpDriverPhase::Stopped) {
                (void)beginClose(IoUringTcpDriverCloseReason::Destroyed);
            }
        } catch (...) {
        }
        pump_.reset();
    }

    IoUringTcpDriverStartResult start() {
        assertOwner();
        if (phase_ == IoUringTcpDriverPhase::Running) {
            return IoUringTcpDriverStartResult::AlreadyStarted;
        }
        if (phase_ != IoUringTcpDriverPhase::Created) {
            return IoUringTcpDriverStartResult::RejectedClosing;
        }
        phase_ = IoUringTcpDriverPhase::Running;
        readDesired_ = true;
        if (submitReceive()) {
            return IoUringTcpDriverStartResult::Accepted;
        }
        ++metrics_.engineRejections;
        (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
        return IoUringTcpDriverStartResult::EngineRejected;
    }

    IoUringTcpDriverSendResult send(std::string_view payload) {
        assertOwner();
        if (phase_ == IoUringTcpDriverPhase::Created) {
            return IoUringTcpDriverSendResult::RejectedNotRunning;
        }
        if (phase_ != IoUringTcpDriverPhase::Running) {
            return IoUringTcpDriverSendResult::RejectedClosing;
        }
        if (payload.empty()) return IoUringTcpDriverSendResult::EmptyPayload;
        if (sendSegments_.size() >= options_.maxPendingSendSegments) {
            ++metrics_.segmentLimitRejections;
            return IoUringTcpDriverSendResult::SegmentLimit;
        }
        if (payload.size() > options_.maxPendingSendBytes - pendingSendBytes_) {
            ++metrics_.byteLimitRejections;
            return IoUringTcpDriverSendResult::ByteLimit;
        }

        sendSegments_.emplace_back(payload);
        pendingSendBytes_ += payload.size();
        ++metrics_.sendAdmissions;
        if (!sendIdentity_.valid()) {
            const auto outcome = submitFrontSend();
            if (outcome != IoUringSubmissionResult::Accepted) {
                pendingSendBytes_ -= sendSegments_.back().size();
                sendSegments_.pop_back();
                --metrics_.sendAdmissions;
                updateSendQueueMetrics();
                ++metrics_.engineRejections;
                if (outcome == IoUringSubmissionResult::RejectedQuiescing ||
                    outcome == IoUringSubmissionResult::RejectedShutdown) {
                    (void)beginClose(
                        IoUringTcpDriverCloseReason::EventLoopQuiescing);
                }
                return IoUringTcpDriverSendResult::EngineRejected;
            }
        }
        updateSendQueueMetrics();
        return IoUringTcpDriverSendResult::Accepted;
    }

    IoUringTcpDriverReadControlResult pauseRead() {
        assertOwner();
        if (phase_ == IoUringTcpDriverPhase::Created) {
            return IoUringTcpDriverReadControlResult::RejectedNotRunning;
        }
        if (phase_ != IoUringTcpDriverPhase::Running) {
            return IoUringTcpDriverReadControlResult::RejectedClosing;
        }
        if (!readDesired_) {
            return IoUringTcpDriverReadControlResult::Unchanged;
        }
        readDesired_ = false;
        ++metrics_.readPauses;
        if (receiveIdentity_.valid()) {
            const auto result = pump_->cancel(receiveIdentity_);
            if (result == IoUringCancelResult::Accepted ||
                result == IoUringCancelResult::AlreadyRequested) {
                ++metrics_.receiveCancelRequests;
            } else if (result != IoUringCancelResult::SubmissionQueueFull) {
                ++metrics_.invariantFailures;
                (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
            }
        }
        return IoUringTcpDriverReadControlResult::Applied;
    }

    IoUringTcpDriverReadControlResult resumeRead() {
        assertOwner();
        if (phase_ == IoUringTcpDriverPhase::Created) {
            return IoUringTcpDriverReadControlResult::RejectedNotRunning;
        }
        if (phase_ != IoUringTcpDriverPhase::Running) {
            return IoUringTcpDriverReadControlResult::RejectedClosing;
        }
        if (readDesired_) {
            return IoUringTcpDriverReadControlResult::Unchanged;
        }
        readDesired_ = true;
        ++metrics_.readResumes;
        if (!receiveIdentity_.valid() && !submitReceive()) {
            ++metrics_.engineRejections;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
        }
        return IoUringTcpDriverReadControlResult::Applied;
    }

    bool close(IoUringTcpDriverCloseReason reason) {
        assertOwner();
        return beginClose(reason);
    }

    IoUringTcpDriverPhase phase() const noexcept { return phase_; }

    bool readPaused() const noexcept { return !readDesired_; }

    IoUringTcpConnectionDriverMetrics metrics() const noexcept {
        auto snapshot = metrics_;
        snapshot.activeReceives = receiveIdentity_.valid() ? 1U : 0U;
        snapshot.activeSends = sendIdentity_.valid() ? 1U : 0U;
        snapshot.pendingSendBytes = pendingSendBytes_;
        snapshot.pendingSendSegments = sendSegments_.size();
        snapshot.foreignMutationRejections =
            foreignMutationRejections_.load(std::memory_order_relaxed);
        return snapshot;
    }

    std::shared_future<IoUringTcpConnectionDriverStopSummary> stopFuture() const {
        return stopFuture_;
    }

private:
    void assertOwner() const {
        if (!ownerLoop_->isInLoopThread()) {
            foreignMutationRejections_.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error(
                "IoUringTcpConnectionDriver used from a different thread");
        }
    }

    bool submitReceive() {
        if (phase_ != IoUringTcpDriverPhase::Running || !readDesired_ ||
            receiveIdentity_.valid()) {
            return true;
        }
        const auto outcome = pump_->enqueueRecv(
            socket_.fd(),
            options_.maxReceiveBytes);
        if (outcome.result != IoUringSubmissionResult::Accepted) return false;
        receiveIdentity_ = outcome.identity;
        ++metrics_.receiveSubmissions;
        metrics_.activeReceives = 1;
        metrics_.maxActiveReceives =
            (std::max)(metrics_.maxActiveReceives, std::size_t{1});
        return true;
    }

    IoUringSubmissionResult submitFrontSend() {
        if (sendSegments_.empty() || sendIdentity_.valid()) {
            return IoUringSubmissionResult::Accepted;
        }
        const auto& front = sendSegments_.front();
        const auto size =
            (std::min)(front.size(), options_.maxSendBytesPerOperation);
        const auto outcome = pump_->enqueueSend(
            socket_.fd(),
            std::string_view(front.data(), size));
        if (outcome.result == IoUringSubmissionResult::Accepted) {
            sendIdentity_ = outcome.identity;
            ++metrics_.sendSubmissions;
            metrics_.activeSends = 1;
            metrics_.maxActiveSends =
                (std::max)(metrics_.maxActiveSends, std::size_t{1});
        }
        return outcome.result;
    }

    bool beginClose(IoUringTcpDriverCloseReason reason) {
        if (phase_ == IoUringTcpDriverPhase::Closing ||
            phase_ == IoUringTcpDriverPhase::Stopped) {
            ++metrics_.duplicateCloseRequests;
            return false;
        }
        closeReason_ = reason;
        phase_ = IoUringTcpDriverPhase::Closing;
        readDesired_ = false;
        ++metrics_.closeRequests;
        discardQueuedSendSegmentsAfterActive();
        pump_->beginQuiesce();
        return true;
    }

    void handleNotice(IoUringCompletionNotice& notice) noexcept {
        ++callbackDepth_;
        metrics_.maxCallbackDepth =
            (std::max)(metrics_.maxCallbackDepth, callbackDepth_);
        if (ownerLoop_->phase() != gamenet::net::EventLoopPhase::Running &&
            phase_ != IoUringTcpDriverPhase::Closing &&
            phase_ != IoUringTcpDriverPhase::Stopped) {
            (void)beginClose(
                IoUringTcpDriverCloseReason::EventLoopQuiescing);
        }
        try {
            switch (notice.kind()) {
            case IoUringOperationKind::Receive:
                handleReceiveNotice(notice);
                break;
            case IoUringOperationKind::Send:
                handleSendNotice(notice);
                break;
            case IoUringOperationKind::Accept:
                ++metrics_.invariantFailures;
                (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
                break;
            }
        } catch (...) {
            ++metrics_.callbackFailures;
            (void)beginClose(IoUringTcpDriverCloseReason::CallbackFailed);
        }
        --callbackDepth_;
    }

    void handleReceiveNotice(IoUringCompletionNotice& notice) {
        if (!receiveIdentity_.valid() ||
            !sameIdentity(receiveIdentity_, notice.identity())) {
            ++metrics_.invariantFailures;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
            return;
        }
        receiveIdentity_ = {};
        metrics_.activeReceives = 0;
        ++metrics_.receiveTerminals;
        if (notice.status() == IoUringCompletionStatus::Cancelled) {
            ++metrics_.receiveCancellations;
            if (phase_ == IoUringTcpDriverPhase::Running && readDesired_ &&
                !submitReceive()) {
                ++metrics_.engineRejections;
                (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
            }
            return;
        }
        if (phase_ == IoUringTcpDriverPhase::Closing) return;
        if (notice.status() != IoUringCompletionStatus::Succeeded) {
            (void)beginClose(IoUringTcpDriverCloseReason::ReceiveFailed);
            return;
        }
        if (notice.bytesTransferred() == 0) {
            (void)beginClose(IoUringTcpDriverCloseReason::PeerClosed);
            return;
        }
        if (notice.bytesTransferred() != notice.payload().size()) {
            ++metrics_.invariantFailures;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
            return;
        }
        ++metrics_.messagesDelivered;
        metrics_.bytesReceived += notice.bytesTransferred();
        try {
            messageConsumer_(notice.payload());
        } catch (...) {
            ++metrics_.callbackFailures;
            (void)beginClose(IoUringTcpDriverCloseReason::CallbackFailed);
            return;
        }
        if (phase_ == IoUringTcpDriverPhase::Running && readDesired_ &&
            !receiveIdentity_.valid() && !submitReceive()) {
            ++metrics_.engineRejections;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
        }
    }

    void handleSendNotice(IoUringCompletionNotice& notice) {
        if (!sendIdentity_.valid() ||
            !sameIdentity(sendIdentity_, notice.identity()) ||
            sendSegments_.empty()) {
            ++metrics_.invariantFailures;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
            return;
        }
        sendIdentity_ = {};
        metrics_.activeSends = 0;
        ++metrics_.sendTerminals;
        auto& front = sendSegments_.front();
        const auto submittedBytes =
            (std::min)(front.size(), options_.maxSendBytesPerOperation);
        const std::string_view expected(front.data(), submittedBytes);
        const bool validSuccess =
            notice.status() == IoUringCompletionStatus::Succeeded &&
            notice.bytesTransferred() != 0 &&
            notice.bytesTransferred() <= submittedBytes &&
            notice.payload() == expected;
        if (!validSuccess) {
            discardFrontSendSegment();
            if (phase_ != IoUringTcpDriverPhase::Closing) {
                (void)beginClose(IoUringTcpDriverCloseReason::SendFailed);
            }
            return;
        }

        const auto transferred = notice.bytesTransferred();
        metrics_.bytesSent += transferred;
        pendingSendBytes_ -= transferred;
        front.erase(0, transferred);
        if (front.empty()) sendSegments_.pop_front();
        updateSendQueueMetrics();

        if (phase_ == IoUringTcpDriverPhase::Closing) {
            discardAllSendSegments();
            return;
        }
        if (!sendSegments_.empty() &&
            submitFrontSend() != IoUringSubmissionResult::Accepted) {
            ++metrics_.engineRejections;
            (void)beginClose(IoUringTcpDriverCloseReason::EngineRejected);
        }
    }

    void discardFrontSendSegment() noexcept {
        if (sendSegments_.empty()) return;
        const auto bytes = sendSegments_.front().size();
        metrics_.bytesDiscarded += bytes;
        pendingSendBytes_ -= bytes;
        sendSegments_.pop_front();
        updateSendQueueMetrics();
    }

    void discardQueuedSendSegmentsAfterActive() noexcept {
        const std::size_t retained = sendIdentity_.valid() ? 1U : 0U;
        while (sendSegments_.size() > retained) {
            const auto bytes = sendSegments_.back().size();
            metrics_.bytesDiscarded += bytes;
            pendingSendBytes_ -= bytes;
            sendSegments_.pop_back();
        }
        updateSendQueueMetrics();
    }

    void discardAllSendSegments() noexcept {
        while (!sendSegments_.empty()) discardFrontSendSegment();
    }

    void updateSendQueueMetrics() noexcept {
        metrics_.pendingSendBytes = pendingSendBytes_;
        metrics_.pendingSendSegments = sendSegments_.size();
        metrics_.maxPendingSendBytes =
            (std::max)(metrics_.maxPendingSendBytes, pendingSendBytes_);
        metrics_.maxPendingSendSegments = (std::max)(
            metrics_.maxPendingSendSegments,
            sendSegments_.size());
    }

    void handlePumpStopped(
        const IoUringEventLoopPumpStopSummary& pumpSummary) noexcept {
        if (phase_ != IoUringTcpDriverPhase::Closing) {
            closeReason_ =
                ownerLoop_->phase() == gamenet::net::EventLoopPhase::Running
                ? IoUringTcpDriverCloseReason::EngineRejected
                : IoUringTcpDriverCloseReason::EventLoopQuiescing;
            phase_ = IoUringTcpDriverPhase::Closing;
            ++metrics_.closeRequests;
        }
        if (receiveIdentity_.valid() || sendIdentity_.valid()) {
            ++metrics_.invariantFailures;
            receiveIdentity_ = {};
            sendIdentity_ = {};
        }
        discardAllSendSegments();
        if (gamenet::net::sockets::isValid(socket_.fd())) {
            socket_.close();
            ++metrics_.socketCloseCount;
        }
        phase_ = IoUringTcpDriverPhase::Stopped;
        if (closeConsumer_) {
            try {
                closeConsumer_(closeReason_);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
        if (!stopPublished_) {
            stopPublished_ = true;
            stopPromise_.set_value(IoUringTcpConnectionDriverStopSummary{
                .reason = closeReason_,
                .driver = metrics(),
                .pump = pumpSummary,
                .socketClosed =
                    !gamenet::net::sockets::isValid(socket_.fd()),
            });
        }
    }

    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::Socket socket_;
    IoUringTcpConnectionDriverOptions options_;
    IoUringTcpConnectionDriver::MessageConsumer messageConsumer_;
    IoUringTcpConnectionDriver::CloseConsumer closeConsumer_;
    std::promise<IoUringTcpConnectionDriverStopSummary> stopPromise_;
    std::shared_future<IoUringTcpConnectionDriverStopSummary> stopFuture_;
    std::unique_ptr<IoUringEventLoopPump> pump_;
    std::deque<std::string> sendSegments_;
    IoUringOperationIdentity receiveIdentity_{};
    IoUringOperationIdentity sendIdentity_{};
    IoUringTcpConnectionDriverMetrics metrics_{};
    mutable std::atomic<std::uint64_t> foreignMutationRejections_{0};
    IoUringTcpDriverPhase phase_{IoUringTcpDriverPhase::Created};
    IoUringTcpDriverCloseReason closeReason_{
        IoUringTcpDriverCloseReason::Explicit};
    std::size_t pendingSendBytes_{};
    std::size_t callbackDepth_{};
    bool readDesired_{false};
    bool stopPublished_{false};
};

IoUringTcpConnectionDriver::IoUringTcpConnectionDriver(
    gamenet::net::EventLoop* ownerLoop,
    gamenet::net::SocketFd establishedSocket,
    IoUringTcpConnectionDriverOptions options,
    MessageConsumer messageConsumer,
    CloseConsumer closeConsumer)
    : impl_(std::make_unique<IoUringTcpConnectionDriverImpl>(
          ownerLoop,
          establishedSocket,
          options,
          std::move(messageConsumer),
          std::move(closeConsumer))) {}

IoUringTcpConnectionDriver::~IoUringTcpConnectionDriver() = default;

IoUringTcpDriverStartResult IoUringTcpConnectionDriver::start() {
    return impl_->start();
}

IoUringTcpDriverSendResult IoUringTcpConnectionDriver::send(
    std::string_view payload) {
    return impl_->send(payload);
}

IoUringTcpDriverReadControlResult IoUringTcpConnectionDriver::pauseRead() {
    return impl_->pauseRead();
}

IoUringTcpDriverReadControlResult IoUringTcpConnectionDriver::resumeRead() {
    return impl_->resumeRead();
}

bool IoUringTcpConnectionDriver::close(IoUringTcpDriverCloseReason reason) {
    return impl_->close(reason);
}

IoUringTcpDriverPhase IoUringTcpConnectionDriver::phase() const noexcept {
    return impl_->phase();
}

bool IoUringTcpConnectionDriver::readPaused() const noexcept {
    return impl_->readPaused();
}

IoUringTcpConnectionDriverMetrics
IoUringTcpConnectionDriver::metrics() const noexcept {
    return impl_->metrics();
}

std::shared_future<IoUringTcpConnectionDriverStopSummary>
IoUringTcpConnectionDriver::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
