#include "IoUringTcpConnectionAdapter.h"

#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::io_uring {

void IoUringTcpConnectionAdapterOptions::validate() const {
    if (highWaterMarkBytes == 0 ||
        lowWaterMarkBytes >= highWaterMarkBytes ||
        highWaterMarkBytes > hardLimitBytes) {
        throw std::invalid_argument(
            "io_uring TCP adapter requires low < high <= hard limits");
    }
}

namespace {

using CloseInfo = gamenet::net::TcpConnectionCloseInfo;
using ClosePhase = gamenet::net::TcpConnectionClosePhase;

struct AdapterState {
    AdapterState(
        gamenet::net::EventLoop* loopValue,
        IoUringTcpConnectionHub* hubValue,
        IoUringTcpConnectionAdapterOptions optionsValue)
        : loop(loopValue),
          hub(hubValue),
          options(optionsValue),
          future(promise.get_future().share()) {}

    gamenet::net::EventLoop* loop;
    IoUringTcpConnectionHub* hub;
    IoUringTcpConnectionAdapterOptions options;
    IoUringTcpConnectionAdapter* observer{};
    IoUringTcpConnectionIdentity identity{};
    std::shared_future<IoUringTcpConnectionHubConnectionStopSummary>
        transportFuture;
    std::promise<IoUringTcpConnectionAdapterStopSummary> promise;
    std::shared_future<IoUringTcpConnectionAdapterStopSummary> future;
    IoUringTcpConnectionAdapter::MessageCallback messageCallback;
    IoUringTcpConnectionAdapter::HighWaterMarkCallback highWaterCallback;
    IoUringTcpConnectionAdapter::WriteCompleteCallback writeCompleteCallback;
    IoUringTcpConnectionAdapter::CloseInfoCallback closeInfoCallback;
    IoUringTcpConnectionAdapter::CloseCallback closeCallback;
    IoUringTcpConnectionAdapterMetrics metrics{};
    std::optional<CloseInfo> closeInfo;
    ClosePhase phase{ClosePhase::Open};
    bool configured{true};
    bool established{false};
    bool overloadLatched{false};
    bool gracefulRequested{false};
    bool forceCloseRequested{false};
    bool terminalPublished{false};
};

void assertOwner(const AdapterState& state) {
    if (!state.loop->isInLoopThread()) {
        throw std::runtime_error(
            "IoUringTcpConnectionAdapter used from a different thread");
    }
}

CloseInfo mapCloseInfo(
    IoUringTcpHubCloseReason reason,
    int nativeError) noexcept {
    using StableReason = gamenet::net::TcpConnectionCloseReason;
    StableReason mapped = StableReason::InternalError;
    switch (reason) {
    case IoUringTcpHubCloseReason::GracefulShutdown:
        mapped = StableReason::GracefulShutdown;
        break;
    case IoUringTcpHubCloseReason::PeerClosed:
        mapped = StableReason::PeerEof;
        break;
    case IoUringTcpHubCloseReason::CallbackFailed:
        mapped = StableReason::CallbackFailure;
        break;
    case IoUringTcpHubCloseReason::ReceiveFailed:
    case IoUringTcpHubCloseReason::SendFailed:
        mapped = nativeError == ECONNRESET || nativeError == ECONNABORTED ||
                nativeError == EPIPE
            ? StableReason::Reset
            : StableReason::InternalError;
        break;
    case IoUringTcpHubCloseReason::Explicit:
    case IoUringTcpHubCloseReason::EventLoopQuiescing:
    case IoUringTcpHubCloseReason::HubStopped:
    case IoUringTcpHubCloseReason::Destroyed:
        mapped = StableReason::ForcedShutdown;
        break;
    case IoUringTcpHubCloseReason::EngineRejected:
        mapped = StableReason::InternalError;
        break;
    }
    return {.reason = mapped, .nativeError = nativeError};
}

void publishCloseInfo(
    AdapterState& state,
    CloseInfo closeInfo) noexcept {
    if (!state.closeInfo) state.closeInfo = closeInfo;
}

void requestCallbackFailure(
    const std::shared_ptr<AdapterState>& state) noexcept {
    ++state->metrics.callbackFailures;
    publishCloseInfo(
        *state,
        {.reason = gamenet::net::TcpConnectionCloseReason::CallbackFailure,
         .nativeError = 0});
    if (state->phase != ClosePhase::Closed &&
        !state->forceCloseRequested) {
        state->phase = ClosePhase::Closing;
        state->forceCloseRequested = true;
        try {
            (void)state->hub->closeConnection(
                state->identity,
                IoUringTcpHubCloseReason::CallbackFailed);
        } catch (...) {
        }
    }
}

template <typename Callback>
void queueNotification(
    const std::shared_ptr<AdapterState>& state,
    Callback callback) noexcept {
    try {
        const std::weak_ptr<AdapterState> weak = state;
        if (!state->loop->tryQueueInLoop(
                [weak, callback = std::move(callback)]() mutable {
                    const auto current = weak.lock();
                    if (!current || current->observer == nullptr) return;
                    try {
                        callback(*current, *current->observer);
                    } catch (...) {
                        requestCallbackFailure(current);
                    }
                })) {
            ++state->metrics.droppedNotifications;
        }
    } catch (...) {
        ++state->metrics.droppedNotifications;
    }
}

void handleMessage(
    const std::shared_ptr<AdapterState>& state,
    IoUringTcpConnectionIdentity identity,
    std::string_view payload) {
    if (identity != state->identity || state->observer == nullptr ||
        !state->messageCallback) {
        return;
    }
    try {
        state->messageCallback(*state->observer, payload);
    } catch (...) {
        requestCallbackFailure(state);
    }
}

void handleOutputProgress(
    const std::shared_ptr<AdapterState>& state,
    IoUringTcpConnectionIdentity identity,
    std::size_t pendingBytes) {
    if (identity != state->identity) {
        requestCallbackFailure(state);
        return;
    }

    const auto previous = state->metrics.pendingOutputBytes;
    state->metrics.pendingOutputBytes = pendingBytes;
    if (state->overloadLatched &&
        pendingBytes <= state->options.lowWaterMarkBytes) {
        state->overloadLatched = false;
    }
    if (state->metrics.readingPaused &&
        pendingBytes <= state->options.lowWaterMarkBytes &&
        (state->phase == ClosePhase::Open || state->gracefulRequested)) {
        const auto resumed = state->hub->resumeRead(state->identity);
        if (resumed == IoUringTcpHubReadControlResult::Applied ||
            resumed == IoUringTcpHubReadControlResult::Unchanged) {
            state->metrics.readingPaused = false;
        } else if (resumed == IoUringTcpHubReadControlResult::EngineRejected) {
            publishCloseInfo(
                *state,
                {.reason =
                     gamenet::net::TcpConnectionCloseReason::InternalError,
                 .nativeError = 0});
            state->phase = ClosePhase::Closing;
        }
    }

    if (previous != 0 && pendingBytes == 0 &&
        state->writeCompleteCallback) {
        ++state->metrics.writeCompleteNotifications;
        auto callback = state->writeCompleteCallback;
        queueNotification(
            state,
            [callback = std::move(callback)](
                AdapterState&,
                IoUringTcpConnectionAdapter& observer) mutable {
                callback(observer);
            });
    }
}

void publishTerminal(
    const std::shared_ptr<AdapterState>& state,
    const IoUringTcpConnectionHubConnectionStopSummary& transport) {
    if (state->terminalPublished) return;
    state->terminalPublished = true;
    if (!state->closeInfo) {
        publishCloseInfo(
            *state,
            mapCloseInfo(transport.reason, transport.nativeError));
    }
    state->phase = ClosePhase::Closed;
    state->metrics.pendingOutputBytes = 0;
    state->metrics.readingPaused = false;
    state->promise.set_value({
        .closeInfo = *state->closeInfo,
        .adapter = state->metrics,
        .transport = transport,
        .established = state->established,
    });

    if (state->observer == nullptr) return;
    if (state->closeInfoCallback) {
        auto callback = state->closeInfoCallback;
        try {
            callback(*state->observer, *state->closeInfo);
        } catch (...) {
            ++state->metrics.callbackFailures;
        }
    }
    if (state->observer != nullptr && state->closeCallback) {
        auto callback = state->closeCallback;
        try {
            callback(*state->observer);
        } catch (...) {
            ++state->metrics.callbackFailures;
        }
    }
}

void publishRejected(
    const std::shared_ptr<AdapterState>& state,
    IoUringTcpHubAddResult result) {
    if (state->terminalPublished) return;
    state->phase = ClosePhase::Closed;
    publishCloseInfo(
        *state,
        {.reason = result == IoUringTcpHubAddResult::RejectedShutdown ||
                 result == IoUringTcpHubAddResult::RejectedQuiescing
             ? gamenet::net::TcpConnectionCloseReason::ForcedShutdown
             : gamenet::net::TcpConnectionCloseReason::InternalError,
         .nativeError = 0});
    state->terminalPublished = true;
    state->promise.set_value({
        .closeInfo = *state->closeInfo,
        .adapter = state->metrics,
        .transport = {},
        .established = false,
    });
}

gamenet::net::TcpSendResult mapSendResult(
    IoUringTcpHubSendResult result) noexcept {
    using StableResult = gamenet::net::TcpSendResult;
    switch (result) {
    case IoUringTcpHubSendResult::Accepted:
    case IoUringTcpHubSendResult::EmptyPayload:
        return StableResult::Accepted;
    case IoUringTcpHubSendResult::ConnectionByteLimit:
    case IoUringTcpHubSendResult::ConnectionSegmentLimit:
        return StableResult::Overloaded;
    case IoUringTcpHubSendResult::HubByteLimit:
        return StableResult::LoopOverloaded;
    case IoUringTcpHubSendResult::StaleConnection:
    case IoUringTcpHubSendResult::Closing:
        return StableResult::Closed;
    case IoUringTcpHubSendResult::EngineRejected:
        return StableResult::OwnerUnavailable;
    }
    return StableResult::OwnerUnavailable;
}

}  // namespace

class IoUringTcpConnectionAdapterImpl final {
public:
    IoUringTcpConnectionAdapterImpl(
        IoUringTcpConnectionAdapter* facade,
        gamenet::net::EventLoop* ownerLoop,
        IoUringTcpConnectionHub* hub,
        IoUringTcpConnectionAdapterOptions options)
        : state_(std::make_shared<AdapterState>(ownerLoop, hub, options)) {
        if (facade == nullptr || ownerLoop == nullptr || hub == nullptr) {
            throw std::invalid_argument(
                "io_uring TCP adapter requires facade, owner, and Hub");
        }
        options.validate();
        ownerLoop->assertInLoopThread();
        state_->observer = facade;
    }

    ~IoUringTcpConnectionAdapterImpl() {
        if (!state_) return;
        if (!state_->loop->isInLoopThread()) std::terminate();
        state_->observer = nullptr;
        if (state_->established && state_->phase != ClosePhase::Closed) {
            publishCloseInfo(
                *state_,
                {.reason =
                     gamenet::net::TcpConnectionCloseReason::ForcedShutdown,
                 .nativeError = 0});
            state_->phase = ClosePhase::Closing;
            (void)state_->hub->closeConnection(
                state_->identity,
                IoUringTcpHubCloseReason::Destroyed);
        } else if (!state_->established && !state_->terminalPublished) {
            publishRejected(
                state_,
                IoUringTcpHubAddResult::RejectedInvalid);
        }
    }

    template <typename Callback>
    void configure(Callback AdapterState::*member, Callback callback) {
        assertOwner(*state_);
        if (!state_->configured) {
            throw std::logic_error(
                "io_uring TCP adapter callbacks are sealed after establish");
        }
        state_.get()->*member = std::move(callback);
    }

    IoUringTcpHubAddResult establish(gamenet::net::SocketFd socket) {
        assertOwner(*state_);
        if (!state_->configured) return IoUringTcpHubAddResult::RejectedInvalid;
        state_->configured = false;
        const auto state = state_;
        auto outcome = state_->hub->addConnection(
            socket,
            [state](IoUringTcpConnectionIdentity identity,
                    std::string_view payload) {
                handleMessage(state, identity, payload);
            },
            [state](IoUringTcpConnectionIdentity identity,
                    IoUringTcpHubCloseReason) {
                if (identity != state->identity ||
                    !state->transportFuture.valid()) {
                    requestCallbackFailure(state);
                    return;
                }
                publishTerminal(state, state->transportFuture.get());
            },
            [state](IoUringTcpConnectionIdentity identity,
                    std::size_t pendingBytes) {
                handleOutputProgress(state, identity, pendingBytes);
            });
        if (outcome.result != IoUringTcpHubAddResult::Accepted) {
            publishRejected(state_, outcome.result);
            return outcome.result;
        }
        state_->identity = outcome.identity;
        state_->transportFuture = std::move(outcome.stopFuture);
        state_->established = true;
        state_->phase = ClosePhase::Open;
        return outcome.result;
    }

    gamenet::net::TcpSendResult trySend(std::string_view payload) {
        assertOwner(*state_);
        if (!state_->established || state_->phase != ClosePhase::Open) {
            return gamenet::net::TcpSendResult::Closed;
        }
        if (payload.empty()) return gamenet::net::TcpSendResult::Accepted;

        auto& metrics = state_->metrics;
        const auto pending = metrics.pendingOutputBytes;
        if (state_->overloadLatched) {
            if (pending > state_->options.lowWaterMarkBytes) {
                ++metrics.overloadRejections;
                return gamenet::net::TcpSendResult::Overloaded;
            }
            state_->overloadLatched = false;
        }
        if (pending > state_->options.hardLimitBytes ||
            payload.size() > state_->options.hardLimitBytes - pending) {
            if (pending > state_->options.lowWaterMarkBytes) {
                state_->overloadLatched = true;
            }
            ++metrics.overloadRejections;
            return gamenet::net::TcpSendResult::Overloaded;
        }

        const auto result = state_->hub->send(state_->identity, payload);
        const auto mapped = mapSendResult(result);
        if (mapped != gamenet::net::TcpSendResult::Accepted) {
            if (mapped == gamenet::net::TcpSendResult::Overloaded) {
                ++metrics.overloadRejections;
            } else if (mapped == gamenet::net::TcpSendResult::LoopOverloaded) {
                ++metrics.ownerLimitRejections;
            }
            return mapped;
        }

        const auto oldPending = metrics.pendingOutputBytes;
        metrics.pendingOutputBytes += payload.size();
        metrics.peakPendingOutputBytes = (std::max)(
            metrics.peakPendingOutputBytes,
            metrics.pendingOutputBytes);
        ++metrics.sendAdmissions;
        if (!metrics.readingPaused &&
            oldPending < state_->options.highWaterMarkBytes &&
            metrics.pendingOutputBytes >=
                state_->options.highWaterMarkBytes) {
            const auto paused = state_->hub->pauseRead(state_->identity);
            if (paused == IoUringTcpHubReadControlResult::Applied ||
                paused == IoUringTcpHubReadControlResult::Unchanged) {
                metrics.readingPaused = true;
                if (state_->highWaterCallback) {
                    ++metrics.highWaterNotifications;
                    auto callback = state_->highWaterCallback;
                    const auto bytes = metrics.pendingOutputBytes;
                    queueNotification(
                        state_,
                        [callback = std::move(callback), bytes](
                            AdapterState&,
                            IoUringTcpConnectionAdapter& observer) mutable {
                            callback(observer, bytes);
                        });
                }
            }
        }
        return mapped;
    }

    gamenet::net::PostResult tryForceClose() {
        assertOwner(*state_);
        if (!state_->established || state_->phase == ClosePhase::Closed) {
            return gamenet::net::PostResult::Shutdown;
        }
        publishCloseInfo(
            *state_,
            {.reason = gamenet::net::TcpConnectionCloseReason::ForcedShutdown,
             .nativeError = 0});
        if (state_->phase == ClosePhase::Closing &&
            (!state_->gracefulRequested || state_->forceCloseRequested)) {
            return gamenet::net::PostResult::Accepted;
        }
        state_->phase = ClosePhase::Closing;
        state_->forceCloseRequested = true;
        return state_->hub->closeConnection(
                   state_->identity,
                   IoUringTcpHubCloseReason::Explicit)
            ? gamenet::net::PostResult::Accepted
            : gamenet::net::PostResult::OwnerUnavailable;
    }

    gamenet::net::PostResult tryShutdown() {
        assertOwner(*state_);
        if (!state_->established || state_->phase == ClosePhase::Closed) {
            return gamenet::net::PostResult::Shutdown;
        }
        publishCloseInfo(
            *state_,
            {.reason =
                 gamenet::net::TcpConnectionCloseReason::GracefulShutdown,
             .nativeError = 0});
        if (state_->gracefulRequested) {
            return gamenet::net::PostResult::Accepted;
        }
        state_->gracefulRequested = true;
        state_->phase = ClosePhase::Closing;
        return state_->hub->shutdownConnection(state_->identity)
            ? gamenet::net::PostResult::Accepted
            : gamenet::net::PostResult::OwnerUnavailable;
    }

    const std::shared_ptr<AdapterState>& state() const noexcept {
        return state_;
    }

private:
    std::shared_ptr<AdapterState> state_;
};

IoUringTcpConnectionAdapter::IoUringTcpConnectionAdapter(
    gamenet::net::EventLoop* ownerLoop,
    IoUringTcpConnectionHub* hub,
    IoUringTcpConnectionAdapterOptions options)
    : impl_(std::make_unique<IoUringTcpConnectionAdapterImpl>(
          this,
          ownerLoop,
          hub,
          options)) {}

IoUringTcpConnectionAdapter::~IoUringTcpConnectionAdapter() = default;

void IoUringTcpConnectionAdapter::setMessageCallback(
    MessageCallback callback) {
    impl_->configure(&AdapterState::messageCallback, std::move(callback));
}

void IoUringTcpConnectionAdapter::setHighWaterMarkCallback(
    HighWaterMarkCallback callback) {
    impl_->configure(&AdapterState::highWaterCallback, std::move(callback));
}

void IoUringTcpConnectionAdapter::setWriteCompleteCallback(
    WriteCompleteCallback callback) {
    impl_->configure(&AdapterState::writeCompleteCallback, std::move(callback));
}

void IoUringTcpConnectionAdapter::setCloseInfoCallback(
    CloseInfoCallback callback) {
    impl_->configure(&AdapterState::closeInfoCallback, std::move(callback));
}

void IoUringTcpConnectionAdapter::setCloseCallback(
    CloseCallback callback) {
    impl_->configure(&AdapterState::closeCallback, std::move(callback));
}

IoUringTcpHubAddResult IoUringTcpConnectionAdapter::establish(
    gamenet::net::SocketFd establishedSocket) {
    return impl_->establish(establishedSocket);
}

gamenet::net::TcpSendResult IoUringTcpConnectionAdapter::trySend(
    std::string_view payload) {
    return impl_->trySend(payload);
}

gamenet::net::PostResult IoUringTcpConnectionAdapter::tryForceClose() {
    return impl_->tryForceClose();
}

gamenet::net::PostResult IoUringTcpConnectionAdapter::tryShutdown() {
    return impl_->tryShutdown();
}

bool IoUringTcpConnectionAdapter::connected() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.established && state.phase == ClosePhase::Open;
}

bool IoUringTcpConnectionAdapter::disconnected() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.phase == ClosePhase::Closed;
}

std::size_t IoUringTcpConnectionAdapter::pendingOutputBytes() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.metrics.pendingOutputBytes;
}

bool IoUringTcpConnectionAdapter::readingPausedByBackpressure() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.metrics.readingPaused;
}

gamenet::net::TcpConnectionClosePhase
IoUringTcpConnectionAdapter::closePhase() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.phase;
}

std::optional<gamenet::net::TcpConnectionCloseInfo>
IoUringTcpConnectionAdapter::closeInfo() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.closeInfo;
}

IoUringTcpConnectionAdapterMetrics
IoUringTcpConnectionAdapter::metrics() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.metrics;
}

std::shared_future<IoUringTcpConnectionAdapterStopSummary>
IoUringTcpConnectionAdapter::stopFuture() const {
    return impl_->state()->future;
}

}  // namespace gamenet::experimental::io_uring
