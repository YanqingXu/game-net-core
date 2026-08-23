// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "IoUringTcpClient.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::io_uring {

using namespace std::chrono_literals;

void IoUringTcpClientOptions::validate() const {
    connection.validate();
    if (initialRetryDelay <= Duration::zero() ||
        maximumRetryDelay < initialRetryDelay ||
        (connectTimeout && *connectTimeout < Duration::zero())) {
        throw std::invalid_argument(
            "IOE-X13 Client requires positive bounded retry and non-negative timeout");
    }
}

namespace {

std::uint64_t nextGeneration(std::uint64_t current) noexcept {
    ++current;
    if (current == 0) ++current;
    return current;
}

IoUringTcpClientConnectResult mapHubConnectResult(
    IoUringTcpHubConnectResult result) noexcept {
    switch (result) {
    case IoUringTcpHubConnectResult::Accepted:
        return IoUringTcpClientConnectResult::Accepted;
    case IoUringTcpHubConnectResult::AlreadyConnecting:
        return IoUringTcpClientConnectResult::AlreadyActive;
    case IoUringTcpHubConnectResult::RejectedQuiescing:
        return IoUringTcpClientConnectResult::RejectedQuiescing;
    case IoUringTcpHubConnectResult::RejectedShutdown:
        return IoUringTcpClientConnectResult::RejectedShutdown;
    case IoUringTcpHubConnectResult::RejectedInvalid:
    case IoUringTcpHubConnectResult::EngineRejected:
        return IoUringTcpClientConnectResult::EngineRejected;
    }
    return IoUringTcpClientConnectResult::EngineRejected;
}

}  // namespace

class IoUringTcpClientImpl final {
private:
    using Adapter = IoUringTcpConnectionAdapter;

public:
    IoUringTcpClientImpl(
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::InetAddress serverAddress,
        IoUringTcpClientOptions options,
        IoUringTcpClient::StoppedConsumer stoppedConsumer)
        : ownerLoop_(ownerLoop),
          serverAddress_(std::move(serverAddress)),
          options_(std::move(options)),
          stoppedConsumer_(std::move(stoppedConsumer)),
          retryDelay_(options_.initialRetryDelay),
          stopFuture_(stopPromise_.get_future().share()) {
        if (ownerLoop_ == nullptr) {
            throw std::invalid_argument(
                "IOE-X13 Client requires an owner EventLoop");
        }
        ownerLoop_->assertInLoopThread();
        options_.validate();
        lifecycleSource_ = gamenet::net::detail::
            EventLoopLifecycleRegistry::attachQuiesceParticipant(
                *ownerLoop_, [this] { driveLifecycle(); });
        lifecycleAttached_ = true;
        try {
            hub_ = std::make_unique<IoUringTcpConnectionHub>(
                ownerLoop_,
                options_.hub,
                [this](const IoUringTcpConnectionHubStopSummary& summary) {
                    handleHubStopped(summary);
                });
        } catch (...) {
            gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                *ownerLoop_, lifecycleSource_);
            lifecycleAttached_ = false;
            throw;
        }
    }

    ~IoUringTcpClientImpl() {
        if (!ownerLoop_->isInLoopThread() ||
            phase_ != IoUringTcpClientPhase::Stopped || hub_ ||
            lifecycleAttached_ || provisional_ || connection_ ||
            retryTimer_.valid() || timeoutTimer_.valid() ||
            stopFuture_.wait_for(0ms) != std::future_status::ready) {
            std::terminate();
        }
    }

    template <typename Callback>
    void configure(Callback IoUringTcpClientImpl::*member, Callback callback) {
        assertOwner();
        if (startedOnce_ || phase_ != IoUringTcpClientPhase::Idle) {
            throw std::logic_error(
                "IOE-X13 Client callbacks are sealed after first connect");
        }
        this->*member = std::move(callback);
    }

    void setConnectorEventCallback(
        gamenet::net::ConnectorEventCallback callback) {
        configure(&IoUringTcpClientImpl::connectorEventCallback_,
                  std::move(callback));
    }

    void setConnectionCallback(
        IoUringTcpClient::ConnectionCallback callback) {
        configure(&IoUringTcpClientImpl::connectionCallback_,
                  std::move(callback));
    }

    void setMessageCallback(IoUringTcpClient::MessageCallback callback) {
        configure(&IoUringTcpClientImpl::messageCallback_, std::move(callback));
    }

    void setHighWaterMarkCallback(
        IoUringTcpClient::HighWaterMarkCallback callback) {
        configure(&IoUringTcpClientImpl::highWaterCallback_,
                  std::move(callback));
    }

    void setWriteCompleteCallback(
        IoUringTcpClient::WriteCompleteCallback callback) {
        configure(&IoUringTcpClientImpl::writeCompleteCallback_,
                  std::move(callback));
    }

    void setCloseInfoCallback(
        IoUringTcpClient::CloseInfoCallback callback) {
        configure(&IoUringTcpClientImpl::closeInfoCallback_,
                  std::move(callback));
    }

    IoUringTcpClientConnectOutcome connect() {
        assertOwner();
        ++metrics_.connectRequests;
        if (phase_ == IoUringTcpClientPhase::Quiescing) {
            return {.result =
                        IoUringTcpClientConnectResult::RejectedQuiescing,
                    .generation = generation_};
        }
        if (phase_ == IoUringTcpClientPhase::Stopped) {
            return {.result = IoUringTcpClientConnectResult::RejectedShutdown,
                    .generation = generation_};
        }
        if (phase_ == IoUringTcpClientPhase::Connecting ||
            phase_ == IoUringTcpClientPhase::RetryWaiting ||
            phase_ == IoUringTcpClientPhase::Connected) {
            return {.result = IoUringTcpClientConnectResult::AlreadyActive,
                    .generation = generation_};
        }

        desired_ = true;
        startedOnce_ = true;
        startAfterConnectionClose_ = connection_ != nullptr;
        generation_ = nextGeneration(generation_);
        retryDelay_ = options_.initialRetryDelay;
        if (connection_) {
            return {.result = IoUringTcpClientConnectResult::Accepted,
                    .generation = generation_};
        }
        return startAttempt(generation_);
    }

    IoUringTcpClientConnectOutcome restart() {
        assertOwner();
        ++metrics_.restartRequests;
        if (phase_ == IoUringTcpClientPhase::Quiescing) {
            return {.result =
                        IoUringTcpClientConnectResult::RejectedQuiescing,
                    .generation = generation_};
        }
        if (phase_ == IoUringTcpClientPhase::Stopped) {
            return {.result = IoUringTcpClientConnectResult::RejectedShutdown,
                    .generation = generation_};
        }

        desired_ = true;
        startedOnce_ = true;
        generation_ = nextGeneration(generation_);
        retryDelay_ = options_.initialRetryDelay;
        cancelRetryTimer();
        cancelTimeoutTimer();

        if (activeConnectOperation_.valid()) {
            ++metrics_.connectCancellations;
            (void)hub_->cancelConnect(
                activeConnectOperation_,
                IoUringTcpHubConnectCloseReason::Replaced);
            return {.result = IoUringTcpClientConnectResult::Accepted,
                    .generation = generation_};
        }
        if (connection_) {
            startAfterConnectionClose_ = true;
            (void)connection_->tryForceClose();
            return {.result = IoUringTcpClientConnectResult::Accepted,
                    .generation = generation_};
        }
        phase_ = IoUringTcpClientPhase::Idle;
        signalLifecycle();
        return {.result = IoUringTcpClientConnectResult::Accepted,
                .generation = generation_};
    }

    bool disconnect() {
        assertOwner();
        if (phase_ == IoUringTcpClientPhase::Quiescing ||
            phase_ == IoUringTcpClientPhase::Stopped) {
            return false;
        }
        ++metrics_.disconnectRequests;
        desired_ = false;
        startAfterConnectionClose_ = false;
        generation_ = nextGeneration(generation_);
        cancelRetryTimer();
        cancelTimeoutTimer();
        if (activeConnectOperation_.valid()) {
            ++metrics_.connectCancellations;
            (void)hub_->cancelConnect(
                activeConnectOperation_,
                IoUringTcpHubConnectCloseReason::Explicit);
        } else if (connection_) {
            (void)connection_->tryShutdown();
        } else {
            phase_ = IoUringTcpClientPhase::Idle;
        }
        return true;
    }

    std::shared_future<IoUringTcpClientStopSummary> stop() {
        assertOwner();
        if (phase_ != IoUringTcpClientPhase::Stopped &&
            phase_ != IoUringTcpClientPhase::Quiescing) {
            ++metrics_.stopRequests;
            beginStop();
        }
        return stopFuture_;
    }

    IoUringTcpClientPhase phase() const {
        assertOwner();
        return phase_;
    }

    bool connected() const {
        assertOwner();
        return connection_ && connection_->connected();
    }

    Adapter* connection() const {
        assertOwner();
        return connection_.get();
    }

    IoUringTcpClientMetrics metrics() const {
        assertOwner();
        auto snapshot = metrics_;
        snapshot.activeConnections = connection_ ? 1U : 0U;
        snapshot.activeAttempts = activeConnectOperation_.valid() ? 1U : 0U;
        snapshot.retryTimerPending = retryTimer_.valid();
        snapshot.timeoutTimerPending = timeoutTimer_.valid();
        snapshot.hubDestroyed = hub_ == nullptr;
        return snapshot;
    }

    std::shared_future<IoUringTcpClientStopSummary> stopFuture() const {
        assertOwner();
        return stopFuture_;
    }

private:
    void assertOwner() const {
        if (!ownerLoop_->isInLoopThread()) {
            throw std::runtime_error(
                "IOE-X13 Client used from a different thread");
        }
    }

    void signalLifecycle() noexcept {
        if (lifecycleAttached_) (void)lifecycleSource_.signal();
    }

    void emitEvent(gamenet::net::ConnectorEvent event) noexcept {
        if (!connectorEventCallback_) return;
        try {
            connectorEventCallback_(serverAddress_, event);
        } catch (...) {
            ++metrics_.callbackFailures;
        }
    }

    std::unique_ptr<Adapter> makeProvisionalAdapter(
        std::uint64_t attemptGeneration) {
        auto adapter = std::make_unique<Adapter>(
            ownerLoop_, hub_.get(), options_.connection);
        adapter->setMessageCallback(
            [this](Adapter& current, std::string_view payload) {
                if (!messageCallback_) return;
                try {
                    messageCallback_(current, payload);
                } catch (...) {
                    ++metrics_.callbackFailures;
                    throw;
                }
            });
        adapter->setHighWaterMarkCallback(
            [this](Adapter& current, std::size_t bytes) {
                if (!highWaterCallback_) return;
                try {
                    highWaterCallback_(current, bytes);
                } catch (...) {
                    ++metrics_.callbackFailures;
                    throw;
                }
            });
        adapter->setWriteCompleteCallback([this](Adapter& current) {
            if (!writeCompleteCallback_) return;
            try {
                writeCompleteCallback_(current);
            } catch (...) {
                ++metrics_.callbackFailures;
                throw;
            }
        });
        adapter->setCloseInfoCallback(
            [this](Adapter& current,
                   const gamenet::net::TcpConnectionCloseInfo& closeInfo) {
                if (!closeInfoCallback_) return;
                try {
                    closeInfoCallback_(current, closeInfo);
                } catch (...) {
                    ++metrics_.callbackFailures;
                }
            });
        adapter->setCloseCallback(
            [this, attemptGeneration](Adapter& current) {
                handleConnectionClosed(current, attemptGeneration);
            });
        return adapter;
    }

    IoUringTcpClientConnectOutcome startAttempt(
        std::uint64_t attemptGeneration) {
        if (phase_ == IoUringTcpClientPhase::Quiescing || !hub_ ||
            hub_->phase() != IoUringTcpHubPhase::Running) {
            return {.result =
                        IoUringTcpClientConnectResult::RejectedQuiescing,
                    .generation = attemptGeneration};
        }
        phase_ = IoUringTcpClientPhase::Connecting;
        activeAttemptGeneration_ = attemptGeneration;
        ++metrics_.attemptsStarted;
        emitEvent(gamenet::net::ConnectorEvent::ConnectAttempt);
        if (attemptGeneration != generation_ || !desired_ ||
            phase_ != IoUringTcpClientPhase::Connecting) {
            activeAttemptGeneration_ = 0;
            if (phase_ != IoUringTcpClientPhase::Quiescing &&
                phase_ != IoUringTcpClientPhase::Stopped) {
                phase_ = IoUringTcpClientPhase::Idle;
            }
            signalLifecycle();
            return {.result = IoUringTcpClientConnectResult::Accepted,
                    .generation = attemptGeneration};
        }

        gamenet::net::Socket socket(
            gamenet::net::sockets::createNonblocking(
                serverAddress_.family()));
        if (!gamenet::net::sockets::isValid(socket.fd())) {
            const auto error = gamenet::net::sockets::lastError();
            ++metrics_.socketCreateFailures;
            activeAttemptGeneration_ = 0;
            phase_ = IoUringTcpClientPhase::Idle;
            handleAttemptFailure(
                attemptGeneration,
                gamenet::net::ConnectorEvent::ConnectFailed);
            return {
                .result = IoUringTcpClientConnectResult::SocketCreateFailed,
                .generation = attemptGeneration,
                .nativeError = error,
            };
        }
        ++metrics_.attemptSocketsCreated;

        try {
            provisional_ = makeProvisionalAdapter(attemptGeneration);
        } catch (...) {
            socket.close();
            ++metrics_.attemptSocketsClosed;
            ++metrics_.adapterAdmissionFailures;
            activeAttemptGeneration_ = 0;
            phase_ = IoUringTcpClientPhase::Idle;
            handleAttemptFailure(
                attemptGeneration,
                gamenet::net::ConnectorEvent::ConnectFailed);
            return {.result = IoUringTcpClientConnectResult::EngineRejected,
                    .generation = attemptGeneration};
        }

        const auto outcome = hub_->connect(
            socket.releaseFd(),
            serverAddress_,
            [this, attemptGeneration] {
                if (!provisional_ ||
                    activeAttemptGeneration_ != attemptGeneration) {
                    throw std::logic_error(
                        "IOE-X13 lost provisional Adapter before settlement");
                }
                auto* observer = provisional_.get();
                return provisional_->prepareAcceptedConnection(
                    [this, attemptGeneration, observer](
                        Adapter&,
                        IoUringTcpHubAddResult result) {
                        settleAdapter(
                            attemptGeneration, observer, result);
                    });
            },
            [this, attemptGeneration](
                const IoUringTcpHubConnectStopSummary& summary) {
                handleConnectStopped(attemptGeneration, summary);
            });
        const auto mapped = mapHubConnectResult(outcome.result);
        if (outcome.result != IoUringTcpHubConnectResult::Accepted) {
            return {.result = mapped, .generation = attemptGeneration};
        }
        activeConnectOperation_ = outcome.operation;

        if (options_.connectTimeout &&
            attemptGeneration == generation_ && desired_) {
            if (*options_.connectTimeout ==
                IoUringTcpClientOptions::Duration::zero()) {
                handleConnectTimeout(attemptGeneration);
            } else {
                const auto scheduled = ownerLoop_->tryRunAfter(
                    *options_.connectTimeout,
                    [this, attemptGeneration] {
                        handleConnectTimeout(attemptGeneration);
                    });
                if (scheduled.result == gamenet::net::PostResult::Accepted) {
                    timeoutTimer_ = scheduled.timerId;
                } else {
                    ++metrics_.callbackFailures;
                    (void)hub_->cancelConnect(
                        activeConnectOperation_,
                        IoUringTcpHubConnectCloseReason::EngineRejected);
                }
            }
        }
        return {.result = mapped, .generation = attemptGeneration};
    }

    void settleAdapter(
        std::uint64_t attemptGeneration,
        Adapter* observer,
        IoUringTcpHubAddResult result) {
        assertOwner();
        if (!provisional_ || provisional_.get() != observer ||
            activeAttemptGeneration_ != attemptGeneration) {
            throw std::logic_error(
                "IOE-X13 received stale Adapter settlement");
        }
        if (result != IoUringTcpHubAddResult::Accepted) {
            ++metrics_.adapterAdmissionFailures;
            provisional_.reset();
            return;
        }
        connection_ = std::move(provisional_);
        connectionGeneration_ = attemptGeneration;
        connectionPublished_ = false;
        ++metrics_.connectionsEstablished;
    }

    void handleConnectTimeout(std::uint64_t attemptGeneration) noexcept {
        timeoutTimer_ = {};
        if (phase_ != IoUringTcpClientPhase::Connecting ||
            attemptGeneration != generation_ ||
            attemptGeneration != activeAttemptGeneration_ ||
            !activeConnectOperation_.valid()) {
            return;
        }
        (void)hub_->cancelConnect(
            activeConnectOperation_,
            IoUringTcpHubConnectCloseReason::Timeout);
    }

    void handleConnectStopped(
        std::uint64_t attemptGeneration,
        const IoUringTcpHubConnectStopSummary& summary) noexcept {
        cancelTimeoutTimer();
        metrics_.attemptSocketsClosed += summary.connect.socketCloseCount;
        metrics_.attemptSocketsTransferred +=
            summary.connect.socketTransferCount;
        if (activeAttemptGeneration_ != attemptGeneration) {
            ++metrics_.staleTerminals;
            return;
        }
        activeConnectOperation_ = {};
        activeAttemptGeneration_ = 0;

        if (phase_ == IoUringTcpClientPhase::Quiescing) {
            provisional_.reset();
            signalLifecycle();
            return;
        }
        if (attemptGeneration != generation_) {
            ++metrics_.staleTerminals;
            provisional_.reset();
            if (connection_ && connectionGeneration_ == attemptGeneration) {
                (void)connection_->tryForceClose();
            } else {
                phase_ = IoUringTcpClientPhase::Idle;
                signalLifecycle();
            }
            return;
        }

        if (summary.reason == IoUringTcpHubConnectCloseReason::Connected &&
            connection_ && connectionGeneration_ == attemptGeneration) {
            phase_ = IoUringTcpClientPhase::Connected;
            retryDelay_ = options_.initialRetryDelay;
            ++metrics_.connectSuccesses;
            emitEvent(gamenet::net::ConnectorEvent::ConnectSuccess);
            if (attemptGeneration != generation_ || !desired_ ||
                phase_ != IoUringTcpClientPhase::Connected ||
                !connection_) {
                if (connection_) (void)connection_->tryForceClose();
                return;
            }
            connectionPublished_ = true;
            if (connectionCallback_) {
                try {
                    connectionCallback_(*connection_);
                } catch (...) {
                    ++metrics_.callbackFailures;
                    (void)connection_->tryForceClose();
                }
            }
            return;
        }

        provisional_.reset();
        phase_ = IoUringTcpClientPhase::Idle;
        switch (summary.reason) {
        case IoUringTcpHubConnectCloseReason::Timeout:
            ++metrics_.connectTimeouts;
            handleAttemptFailure(
                attemptGeneration,
                gamenet::net::ConnectorEvent::ConnectTimeout);
            break;
        case IoUringTcpHubConnectCloseReason::Replaced:
        case IoUringTcpHubConnectCloseReason::Explicit:
            if (desired_) signalLifecycle();
            break;
        case IoUringTcpHubConnectCloseReason::ConnectFailed:
        case IoUringTcpHubConnectCloseReason::EngineRejected:
        case IoUringTcpHubConnectCloseReason::CallbackFailed:
            handleAttemptFailure(
                attemptGeneration,
                gamenet::net::ConnectorEvent::ConnectFailed);
            break;
        case IoUringTcpHubConnectCloseReason::EventLoopQuiescing:
        case IoUringTcpHubConnectCloseReason::HubStopped:
        case IoUringTcpHubConnectCloseReason::Destroyed:
            beginStop();
            break;
        case IoUringTcpHubConnectCloseReason::Connected:
            ++metrics_.callbackFailures;
            handleAttemptFailure(
                attemptGeneration,
                gamenet::net::ConnectorEvent::ConnectFailed);
            break;
        }
    }

    void handleAttemptFailure(
        std::uint64_t attemptGeneration,
        gamenet::net::ConnectorEvent event) noexcept {
        ++metrics_.connectFailures;
        emitEvent(event);
        if (attemptGeneration != generation_ ||
            phase_ == IoUringTcpClientPhase::Quiescing || !desired_) {
            return;
        }
        if (options_.retryEnabled) {
            scheduleRetry(attemptGeneration);
            return;
        }
        desired_ = false;
        emitEvent(gamenet::net::ConnectorEvent::TerminalFailure);
    }

    void scheduleRetry(std::uint64_t failedGeneration) noexcept {
        if (failedGeneration != generation_ || !desired_ ||
            phase_ == IoUringTcpClientPhase::Quiescing ||
            phase_ == IoUringTcpClientPhase::Stopped) {
            return;
        }
        phase_ = IoUringTcpClientPhase::RetryWaiting;
        const auto scheduled = ownerLoop_->tryRunAfter(
            retryDelay_,
            [this, failedGeneration] {
                retryTimer_ = {};
                if (phase_ != IoUringTcpClientPhase::RetryWaiting ||
                    failedGeneration != generation_ || !desired_) {
                    return;
                }
                generation_ = nextGeneration(generation_);
                phase_ = IoUringTcpClientPhase::Idle;
                (void)startAttempt(generation_);
            });
        if (scheduled.result != gamenet::net::PostResult::Accepted) {
            ++metrics_.callbackFailures;
            desired_ = false;
            phase_ = IoUringTcpClientPhase::Idle;
            emitEvent(gamenet::net::ConnectorEvent::TerminalFailure);
            return;
        }
        retryTimer_ = scheduled.timerId;
        ++metrics_.retriesScheduled;
        const auto remaining = options_.maximumRetryDelay - retryDelay_;
        retryDelay_ = retryDelay_ >= remaining
            ? options_.maximumRetryDelay
            : retryDelay_ + retryDelay_;
        emitEvent(gamenet::net::ConnectorEvent::RetryScheduled);
    }

    void cancelRetryTimer() noexcept {
        if (!retryTimer_.valid()) return;
        (void)ownerLoop_->tryCancel(retryTimer_);
        retryTimer_ = {};
    }

    void cancelTimeoutTimer() noexcept {
        if (!timeoutTimer_.valid()) return;
        (void)ownerLoop_->tryCancel(timeoutTimer_);
        timeoutTimer_ = {};
    }

    void handleConnectionClosed(
        Adapter& current,
        std::uint64_t connectionGeneration) noexcept {
        if (!connection_ || connection_.get() != &current ||
            connectionGeneration_ != connectionGeneration) {
            ++metrics_.callbackFailures;
            return;
        }
        const bool publishDisconnect = connectionPublished_;
        connectionPublished_ = false;
        if (phase_ != IoUringTcpClientPhase::Quiescing) {
            phase_ = IoUringTcpClientPhase::Idle;
            if (!startAfterConnectionClose_ && !options_.retryEnabled) {
                desired_ = false;
            }
        }
        if (publishDisconnect && connectionCallback_) {
            try {
                connectionCallback_(current);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
        connection_.reset();
        connectionGeneration_ = 0;
        ++metrics_.connectionsRetired;

        if (phase_ == IoUringTcpClientPhase::Quiescing) {
            signalLifecycle();
            return;
        }
        if (startAfterConnectionClose_ && desired_) {
            startAfterConnectionClose_ = false;
            signalLifecycle();
            return;
        }
        if (desired_ && options_.retryEnabled) {
            generation_ = nextGeneration(generation_);
            scheduleRetry(generation_);
        }
    }

    void beginStop() noexcept {
        if (phase_ == IoUringTcpClientPhase::Stopped ||
            phase_ == IoUringTcpClientPhase::Quiescing) {
            return;
        }
        phase_ = IoUringTcpClientPhase::Quiescing;
        desired_ = false;
        startAfterConnectionClose_ = false;
        generation_ = nextGeneration(generation_);
        cancelRetryTimer();
        cancelTimeoutTimer();
        if (hub_) {
            try {
                (void)hub_->stop();
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
        signalLifecycle();
    }

    void handleHubStopped(
        const IoUringTcpConnectionHubStopSummary& summary) noexcept {
        hubSummary_ = summary;
        hubStopped_ = true;
        activeConnectOperation_ = {};
        activeAttemptGeneration_ = 0;
        if (phase_ != IoUringTcpClientPhase::Quiescing &&
            phase_ != IoUringTcpClientPhase::Stopped) {
            phase_ = IoUringTcpClientPhase::Quiescing;
            desired_ = false;
            cancelRetryTimer();
            cancelTimeoutTimer();
        }
        signalLifecycle();
    }

    void driveLifecycle() noexcept {
        try {
            assertOwner();
            if (ownerLoop_->phase() != gamenet::net::EventLoopPhase::Running &&
                phase_ != IoUringTcpClientPhase::Stopped &&
                phase_ != IoUringTcpClientPhase::Quiescing) {
                ++metrics_.stopRequests;
                beginStop();
            }
            if (phase_ == IoUringTcpClientPhase::Idle && desired_ &&
                !connection_ && !provisional_ &&
                !activeConnectOperation_.valid() &&
                !retryTimer_.valid() && hub_ &&
                hub_->phase() == IoUringTcpHubPhase::Running) {
                (void)startAttempt(generation_);
            }
            if (hubStopped_ &&
                phase_ == IoUringTcpClientPhase::Quiescing) {
                provisional_.reset();
                connection_.reset();
                hub_.reset();
                if (lifecycleAttached_) {
                    gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                        *ownerLoop_, lifecycleSource_);
                    lifecycleAttached_ = false;
                }
                phase_ = IoUringTcpClientPhase::Stopped;
                auto snapshot = metrics();
                const auto summary = IoUringTcpClientStopSummary{
                    .client = snapshot,
                    .hub = hubSummary_.value(),
                    .allTimersRetired =
                        !retryTimer_.valid() && !timeoutTimer_.valid(),
                    .allAttemptsRetired =
                        !activeConnectOperation_.valid() && !provisional_,
                    .allConnectionsStopped = !connection_ &&
                        hubSummary_->allConnectionsStopped,
                    .ownerDestroyedHub = true,
                };
                if (!stopPublished_) {
                    stopPublished_ = true;
                    stopPromise_.set_value(summary);
                }
                if (stoppedConsumer_) {
                    try {
                        stoppedConsumer_(summary);
                    } catch (...) {
                        ++metrics_.callbackFailures;
                    }
                }
                return;
            }
            if (phase_ != IoUringTcpClientPhase::Stopped &&
                ownerLoop_->phase() != gamenet::net::EventLoopPhase::Running) {
                signalLifecycle();
            }
        } catch (...) {
            ++metrics_.callbackFailures;
            if (phase_ != IoUringTcpClientPhase::Quiescing &&
                phase_ != IoUringTcpClientPhase::Stopped) {
                beginStop();
            }
            signalLifecycle();
        }
    }

    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::InetAddress serverAddress_;
    IoUringTcpClientOptions options_;
    IoUringTcpClient::StoppedConsumer stoppedConsumer_;
    gamenet::net::ConnectorEventCallback connectorEventCallback_;
    IoUringTcpClient::ConnectionCallback connectionCallback_;
    IoUringTcpClient::MessageCallback messageCallback_;
    IoUringTcpClient::HighWaterMarkCallback highWaterCallback_;
    IoUringTcpClient::WriteCompleteCallback writeCompleteCallback_;
    IoUringTcpClient::CloseInfoCallback closeInfoCallback_;
    std::promise<IoUringTcpClientStopSummary> stopPromise_;
    std::shared_future<IoUringTcpClientStopSummary> stopFuture_;
    std::unique_ptr<IoUringTcpConnectionHub> hub_;
    std::unique_ptr<Adapter> provisional_;
    std::unique_ptr<Adapter> connection_;
    gamenet::net::EventLoopLifecycleSource lifecycleSource_;
    gamenet::net::TimerId retryTimer_;
    gamenet::net::TimerId timeoutTimer_;
    std::optional<IoUringTcpConnectionHubStopSummary> hubSummary_;
    IoUringTcpClientMetrics metrics_{};
    IoUringTcpClientOptions::Duration retryDelay_;
    IoUringOperationIdentity activeConnectOperation_{};
    std::uint64_t generation_{};
    std::uint64_t activeAttemptGeneration_{};
    std::uint64_t connectionGeneration_{};
    IoUringTcpClientPhase phase_{IoUringTcpClientPhase::Idle};
    bool desired_{false};
    bool startedOnce_{false};
    bool startAfterConnectionClose_{false};
    bool connectionPublished_{false};
    bool lifecycleAttached_{false};
    bool hubStopped_{false};
    bool stopPublished_{false};
};

IoUringTcpClient::IoUringTcpClient(
    gamenet::net::EventLoop* ownerLoop,
    gamenet::net::InetAddress serverAddress,
    IoUringTcpClientOptions options,
    StoppedConsumer stoppedConsumer)
    : impl_(std::make_unique<IoUringTcpClientImpl>(
          ownerLoop,
          std::move(serverAddress),
          std::move(options),
          std::move(stoppedConsumer))) {}

IoUringTcpClient::~IoUringTcpClient() = default;

void IoUringTcpClient::setConnectorEventCallback(
    gamenet::net::ConnectorEventCallback callback) {
    impl_->setConnectorEventCallback(std::move(callback));
}

void IoUringTcpClient::setConnectionCallback(ConnectionCallback callback) {
    impl_->setConnectionCallback(std::move(callback));
}

void IoUringTcpClient::setMessageCallback(MessageCallback callback) {
    impl_->setMessageCallback(std::move(callback));
}

void IoUringTcpClient::setHighWaterMarkCallback(
    HighWaterMarkCallback callback) {
    impl_->setHighWaterMarkCallback(std::move(callback));
}

void IoUringTcpClient::setWriteCompleteCallback(
    WriteCompleteCallback callback) {
    impl_->setWriteCompleteCallback(std::move(callback));
}

void IoUringTcpClient::setCloseInfoCallback(CloseInfoCallback callback) {
    impl_->setCloseInfoCallback(std::move(callback));
}

IoUringTcpClientConnectOutcome IoUringTcpClient::connect() {
    return impl_->connect();
}

IoUringTcpClientConnectOutcome IoUringTcpClient::restart() {
    return impl_->restart();
}

bool IoUringTcpClient::disconnect() { return impl_->disconnect(); }

std::shared_future<IoUringTcpClientStopSummary> IoUringTcpClient::stop() {
    return impl_->stop();
}

IoUringTcpClientPhase IoUringTcpClient::phase() const {
    return impl_->phase();
}

bool IoUringTcpClient::connected() const { return impl_->connected(); }

IoUringTcpConnectionAdapter* IoUringTcpClient::connection() const {
    return impl_->connection();
}

IoUringTcpClientMetrics IoUringTcpClient::metrics() const {
    return impl_->metrics();
}

std::shared_future<IoUringTcpClientStopSummary>
IoUringTcpClient::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
