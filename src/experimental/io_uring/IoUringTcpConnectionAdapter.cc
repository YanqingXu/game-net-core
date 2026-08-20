#include "IoUringTcpConnectionAdapter.h"

#include "gamenet/core/net/EventLoop.h"

#include "core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace gamenet::experimental::io_uring {

void IoUringTcpConnectionAdapterOptions::validate() const {
    if (highWaterMarkBytes == 0 ||
        lowWaterMarkBytes >= highWaterMarkBytes ||
        highWaterMarkBytes > hardLimitBytes ||
        maxPendingCommands == 0 || maxCommandsPerTurn == 0 ||
        maxCommandsPerTurn > maxPendingCommands) {
        throw std::invalid_argument(
            "io_uring TCP adapter requires low < high <= hard limits and "
            "0 < commands-per-turn <= pending commands");
    }
}

namespace {

using CloseInfo = gamenet::net::TcpConnectionCloseInfo;
using ClosePhase = gamenet::net::TcpConnectionClosePhase;

enum class AdapterCommandKind : std::uint8_t {
    Send,
    Shutdown,
    ForceClose,
};

enum class AdapterAdmissionPhase : std::uint8_t {
    NotEstablished,
    Open,
    GracefulSealed,
    ForceSealed,
    Closed,
};

struct AdapterCommand {
    AdapterCommandKind kind{AdapterCommandKind::Send};
    std::string payload;
    gamenet::net::TcpSendResult* ownerSendResult{};
    gamenet::net::PostResult* ownerPostResult{};
    bool foreign{};
};

struct AdapterCommandState {
    std::mutex mutex;
    std::deque<AdapterCommand> commands;
    AdapterAdmissionPhase phase{AdapterAdmissionPhase::NotEstablished};
    std::optional<CloseInfo> firstCloseInfo;
    std::size_t reservedOutputBytes{};
    std::size_t peakReservedOutputBytes{};
    std::size_t maxPendingCommands{};
    std::uint64_t sendAdmissions{};
    std::uint64_t overloadRejections{};
    std::uint64_t foreignSendAdmissions{};
    std::uint64_t foreignLifecycleAdmissions{};
    std::uint64_t commandQueueRejections{};
    std::uint64_t cancelledCommandBytes{};
    bool overloadLatched{false};
    bool gracefulAdmitted{false};
    bool forceAdmitted{false};
    bool draining{false};
};

struct AdapterState {
    AdapterState(
        gamenet::net::EventLoop* loopValue,
        IoUringTcpConnectionHub* hubValue,
        IoUringTcpConnectionAdapterOptions optionsValue)
        : loop(loopValue),
          hub(hubValue),
          options(optionsValue),
          command(std::make_shared<AdapterCommandState>()),
          future(promise.get_future().share()) {}

    gamenet::net::EventLoop* loop;
    IoUringTcpConnectionHub* hub;
    IoUringTcpConnectionAdapterOptions options;
    std::shared_ptr<AdapterCommandState> command;
    gamenet::net::EventLoopLifecycleSource commandSource;
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
    bool gracefulRequested{false};
    bool forceCloseRequested{false};
    bool terminalPublished{false};
    bool commandSourceAttached{false};
    std::size_t transportPendingOutputBytes{};
};

void assertOwner(const AdapterState& state) {
    if (!state.loop->isInLoopThread()) {
        throw std::runtime_error(
            "IoUringTcpConnectionAdapter used from a different thread");
    }
}

void applyCommandMetricsLocked(
    AdapterState& state,
    const AdapterCommandState& command) noexcept {
    state.metrics.sendAdmissions = command.sendAdmissions;
    state.metrics.overloadRejections = command.overloadRejections;
    state.metrics.foreignSendAdmissions = command.foreignSendAdmissions;
    state.metrics.foreignLifecycleAdmissions =
        command.foreignLifecycleAdmissions;
    state.metrics.commandQueueRejections = command.commandQueueRejections;
    state.metrics.cancelledCommandBytes = command.cancelledCommandBytes;
    state.metrics.pendingOutputBytes = command.reservedOutputBytes;
    state.metrics.peakPendingOutputBytes = command.peakReservedOutputBytes;
    state.metrics.pendingCommands = command.commands.size();
    state.metrics.maxPendingCommands = command.maxPendingCommands;
}

void syncCommandMetrics(AdapterState& state) noexcept {
    std::lock_guard lock(state.command->mutex);
    applyCommandMetricsLocked(state, *state.command);
}

std::optional<CloseInfo> sealCommandState(AdapterState& state) noexcept {
    std::lock_guard lock(state.command->mutex);
    auto& command = *state.command;
    const auto locallyQueuedBytes =
        command.reservedOutputBytes > state.transportPendingOutputBytes
        ? command.reservedOutputBytes - state.transportPendingOutputBytes
        : 0;
    command.cancelledCommandBytes += locallyQueuedBytes;
    command.reservedOutputBytes = 0;
    command.commands.clear();
    command.phase = AdapterAdmissionPhase::Closed;
    command.draining = false;
    applyCommandMetricsLocked(state, command);
    return command.firstCloseInfo;
}

std::size_t releaseReservedOutputBytes(
    AdapterState& state,
    std::size_t bytes) noexcept {
    std::lock_guard lock(state.command->mutex);
    auto& command = *state.command;
    const auto released =
        (std::min)(bytes, command.reservedOutputBytes);
    command.reservedOutputBytes -= released;
    if (command.reservedOutputBytes <= state.options.lowWaterMarkBytes) {
        command.overloadLatched = false;
    }
    applyCommandMetricsLocked(state, command);
    return command.reservedOutputBytes;
}

std::size_t reservedOutputBytes(AdapterState& state) noexcept {
    std::lock_guard lock(state.command->mutex);
    return state.command->reservedOutputBytes;
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
    const auto admittedCloseInfo = sealCommandState(*state);
    if (admittedCloseInfo) {
        publishCloseInfo(*state, *admittedCloseInfo);
    }
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

    const auto previousTransport = state->transportPendingOutputBytes;
    state->transportPendingOutputBytes = pendingBytes;
    const auto completedBytes = previousTransport > pendingBytes
        ? previousTransport - pendingBytes
        : 0;
    const auto previousReserved = reservedOutputBytes(*state);
    const auto pendingReserved =
        releaseReservedOutputBytes(*state, completedBytes);
    if (state->metrics.readingPaused &&
        pendingReserved <= state->options.lowWaterMarkBytes &&
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

    if (previousReserved != 0 && pendingReserved == 0 &&
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
    const auto admittedCloseInfo = sealCommandState(*state);
    if (!state->closeInfo && admittedCloseInfo) {
        publishCloseInfo(*state, *admittedCloseInfo);
    }
    if (!state->closeInfo) {
        publishCloseInfo(
            *state,
            mapCloseInfo(transport.reason, transport.nativeError));
    }
    state->phase = ClosePhase::Closed;
    state->transportPendingOutputBytes = 0;
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
    (void)sealCommandState(*state);
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

void publishAdmittedCloseInfo(AdapterState& state) noexcept {
    std::lock_guard lock(state.command->mutex);
    if (!state.closeInfo && state.command->firstCloseInfo) {
        publishCloseInfo(state, *state.command->firstCloseInfo);
    }
}

void reconcileRejectedCommandSend(
    AdapterState& state,
    std::size_t bytes,
    bool acceptedToCaller) noexcept {
    std::lock_guard lock(state.command->mutex);
    auto& command = *state.command;
    const auto released =
        (std::min)(bytes, command.reservedOutputBytes);
    command.reservedOutputBytes -= released;
    if (acceptedToCaller) {
        command.cancelledCommandBytes += released;
    } else if (command.sendAdmissions != 0) {
        --command.sendAdmissions;
    }
    if (command.reservedOutputBytes <= state.options.lowWaterMarkBytes) {
        command.overloadLatched = false;
    }
    applyCommandMetricsLocked(state, command);
}

gamenet::net::TcpSendResult sendCommandInLoop(
    const std::shared_ptr<AdapterState>& state,
    AdapterCommand& command) {
    auto& current = *state;
    assertOwner(current);
    const bool acceptedToCaller =
        command.foreign || command.ownerSendResult == nullptr;
    if (!current.established || current.phase != ClosePhase::Open) {
        reconcileRejectedCommandSend(
            current,
            command.payload.size(),
            acceptedToCaller);
        return gamenet::net::TcpSendResult::Closed;
    }

    const auto result = current.hub->send(
        current.identity,
        command.payload);
    const auto mapped = mapSendResult(result);
    if (mapped != gamenet::net::TcpSendResult::Accepted) {
        reconcileRejectedCommandSend(
            current,
            command.payload.size(),
            acceptedToCaller);
        if (mapped == gamenet::net::TcpSendResult::LoopOverloaded) {
            ++current.metrics.ownerLimitRejections;
        }
        if (acceptedToCaller && current.phase != ClosePhase::Closed) {
            publishAdmittedCloseInfo(current);
            publishCloseInfo(
                current,
                {.reason =
                     gamenet::net::TcpConnectionCloseReason::InternalError,
                 .nativeError = 0});
            current.phase = ClosePhase::Closing;
            current.forceCloseRequested = true;
            (void)current.hub->closeConnection(
                current.identity,
                IoUringTcpHubCloseReason::EngineRejected);
        }
        return mapped;
    }

    current.transportPendingOutputBytes += command.payload.size();
    syncCommandMetrics(current);
    const auto pending = reservedOutputBytes(current);
    if (!current.metrics.readingPaused &&
        pending >= current.options.highWaterMarkBytes) {
        const auto paused = current.hub->pauseRead(current.identity);
        if (paused == IoUringTcpHubReadControlResult::Applied ||
            paused == IoUringTcpHubReadControlResult::Unchanged) {
            current.metrics.readingPaused = true;
            if (current.highWaterCallback) {
                ++current.metrics.highWaterNotifications;
                auto callback = current.highWaterCallback;
                queueNotification(
                    state,
                    [callback = std::move(callback), pending](
                        AdapterState&,
                        IoUringTcpConnectionAdapter& observer) mutable {
                        callback(observer, pending);
                    });
            }
        }
    }
    return mapped;
}

gamenet::net::PostResult shutdownCommandInLoop(AdapterState& state) {
    assertOwner(state);
    if (!state.established || state.phase == ClosePhase::Closed) {
        return gamenet::net::PostResult::Shutdown;
    }
    publishAdmittedCloseInfo(state);
    publishCloseInfo(
        state,
        {.reason = gamenet::net::TcpConnectionCloseReason::GracefulShutdown,
         .nativeError = 0});
    if (state.gracefulRequested) {
        return gamenet::net::PostResult::Accepted;
    }
    state.gracefulRequested = true;
    state.phase = ClosePhase::Closing;
    return state.hub->shutdownConnection(state.identity)
        ? gamenet::net::PostResult::Accepted
        : gamenet::net::PostResult::OwnerUnavailable;
}

gamenet::net::PostResult forceCloseCommandInLoop(AdapterState& state) {
    assertOwner(state);
    if (!state.established || state.phase == ClosePhase::Closed) {
        return gamenet::net::PostResult::Shutdown;
    }
    publishAdmittedCloseInfo(state);
    publishCloseInfo(
        state,
        {.reason = gamenet::net::TcpConnectionCloseReason::ForcedShutdown,
         .nativeError = 0});
    if (state.phase == ClosePhase::Closing &&
        (!state.gracefulRequested || state.forceCloseRequested)) {
        return gamenet::net::PostResult::Accepted;
    }
    state.phase = ClosePhase::Closing;
    state.forceCloseRequested = true;
    return state.hub->closeConnection(
               state.identity,
               IoUringTcpHubCloseReason::Explicit)
        ? gamenet::net::PostResult::Accepted
        : gamenet::net::PostResult::OwnerUnavailable;
}

void failCommandDrain(
    const std::shared_ptr<AdapterState>& state) noexcept {
    (void)sealCommandState(*state);
    publishAdmittedCloseInfo(*state);
    publishCloseInfo(
        *state,
        {.reason = gamenet::net::TcpConnectionCloseReason::InternalError,
         .nativeError = 0});
    if (state->established && state->phase != ClosePhase::Closed) {
        state->phase = ClosePhase::Closing;
        state->forceCloseRequested = true;
        try {
            (void)state->hub->closeConnection(
                state->identity,
                IoUringTcpHubCloseReason::EngineRejected);
        } catch (...) {
        }
    }
}

void drainAdapterCommands(
    const std::shared_ptr<AdapterState>& state,
    bool drainAll = false) {
    assertOwner(*state);
    {
        std::lock_guard lock(state->command->mutex);
        if (state->command->draining) return;
        state->command->draining = true;
    }

    const auto limit = drainAll
        ? state->options.maxPendingCommands
        : state->options.maxCommandsPerTurn;
    std::size_t drained = 0;
    while (drained < limit) {
        AdapterCommand command;
        {
            std::lock_guard lock(state->command->mutex);
            if (state->command->commands.empty()) break;
            command = std::move(state->command->commands.front());
            state->command->commands.pop_front();
            applyCommandMetricsLocked(*state, *state->command);
        }

        try {
            if (command.kind == AdapterCommandKind::Send) {
                const auto result = sendCommandInLoop(state, command);
                if (command.ownerSendResult != nullptr) {
                    *command.ownerSendResult = result;
                }
            } else if (command.kind == AdapterCommandKind::Shutdown) {
                const auto result = shutdownCommandInLoop(*state);
                if (command.ownerPostResult != nullptr) {
                    *command.ownerPostResult = result;
                }
            } else {
                const auto result = forceCloseCommandInLoop(*state);
                if (command.ownerPostResult != nullptr) {
                    *command.ownerPostResult = result;
                }
            }
        } catch (...) {
            if (command.ownerSendResult != nullptr) {
                *command.ownerSendResult =
                    gamenet::net::TcpSendResult::OwnerUnavailable;
            }
            if (command.ownerPostResult != nullptr) {
                *command.ownerPostResult =
                    gamenet::net::PostResult::OwnerUnavailable;
            }
            failCommandDrain(state);
            return;
        }
        ++drained;
    }

    bool hasMore = false;
    {
        std::lock_guard lock(state->command->mutex);
        state->command->draining = false;
        hasMore = !state->command->commands.empty();
        applyCommandMetricsLocked(*state, *state->command);
    }
    if (hasMore &&
        state->commandSource.signal() != gamenet::net::PostResult::Accepted) {
        failCommandDrain(state);
    }
}

gamenet::net::TcpSendResult mapCommandSignalToSendResult(
    gamenet::net::PostResult result) noexcept {
    switch (result) {
    case gamenet::net::PostResult::Accepted:
        return gamenet::net::TcpSendResult::Accepted;
    case gamenet::net::PostResult::QueueFull:
        return gamenet::net::TcpSendResult::SchedulingQueueFull;
    case gamenet::net::PostResult::Shutdown:
        return gamenet::net::TcpSendResult::OwnerShutdown;
    case gamenet::net::PostResult::OwnerUnavailable:
        return gamenet::net::TcpSendResult::OwnerUnavailable;
    }
    return gamenet::net::TcpSendResult::OwnerUnavailable;
}

gamenet::net::TcpSendResult admitAdapterSend(
    const std::shared_ptr<AdapterState>& state,
    std::string_view payload) {
    const bool foreign = !state->loop->isInLoopThread();
    if (payload.empty()) {
        std::lock_guard lock(state->command->mutex);
        return state->command->phase == AdapterAdmissionPhase::Open
            ? gamenet::net::TcpSendResult::Accepted
            : gamenet::net::TcpSendResult::Closed;
    }

    std::string ownedPayload(payload);
    std::optional<gamenet::net::TcpSendResult> ownerResult;
    gamenet::net::PostResult signalResult =
        gamenet::net::PostResult::Accepted;
    {
        std::lock_guard lock(state->command->mutex);
        auto& command = *state->command;
        if (command.phase != AdapterAdmissionPhase::Open) {
            return gamenet::net::TcpSendResult::Closed;
        }
        if (command.overloadLatched) {
            if (command.reservedOutputBytes >
                state->options.lowWaterMarkBytes) {
                ++command.overloadRejections;
                return gamenet::net::TcpSendResult::Overloaded;
            }
            command.overloadLatched = false;
        }
        if (command.reservedOutputBytes > state->options.hardLimitBytes ||
            ownedPayload.size() >
                state->options.hardLimitBytes -
                    command.reservedOutputBytes) {
            if (command.reservedOutputBytes >
                state->options.lowWaterMarkBytes) {
                command.overloadLatched = true;
            }
            ++command.overloadRejections;
            return gamenet::net::TcpSendResult::Overloaded;
        }
        if (command.commands.size() >=
            state->options.maxPendingCommands) {
            ++command.commandQueueRejections;
            return gamenet::net::TcpSendResult::SchedulingQueueFull;
        }

        AdapterCommand admitted{
            .kind = AdapterCommandKind::Send,
            .payload = std::move(ownedPayload),
            .foreign = foreign,
        };
        if (!foreign && !command.draining) {
            admitted.ownerSendResult = &ownerResult.emplace(
                gamenet::net::TcpSendResult::Accepted);
        }
        try {
            command.commands.push_back(std::move(admitted));
        } catch (...) {
            ++command.commandQueueRejections;
            return gamenet::net::TcpSendResult::SchedulingQueueFull;
        }
        const auto bytes = command.commands.back().payload.size();
        command.reservedOutputBytes += bytes;

        if (foreign) {
            signalResult = state->commandSource.signal();
            if (signalResult != gamenet::net::PostResult::Accepted) {
                command.reservedOutputBytes -= bytes;
                command.commands.pop_back();
                return mapCommandSignalToSendResult(signalResult);
            }
            ++command.foreignSendAdmissions;
        }
        command.peakReservedOutputBytes = (std::max)(
            command.peakReservedOutputBytes,
            command.reservedOutputBytes);
        command.maxPendingCommands = (std::max)(
            command.maxPendingCommands,
            command.commands.size());
        ++command.sendAdmissions;
    }

    if (!foreign) {
        drainAdapterCommands(state, true);
        return ownerResult.value_or(gamenet::net::TcpSendResult::Accepted);
    }
    return gamenet::net::TcpSendResult::Accepted;
}

gamenet::net::PostResult admitAdapterTerminal(
    const std::shared_ptr<AdapterState>& state,
    AdapterCommandKind kind) noexcept {
    const bool foreign = !state->loop->isInLoopThread();
    std::optional<gamenet::net::PostResult> ownerResult;
    gamenet::net::PostResult signalResult =
        gamenet::net::PostResult::Accepted;
    try {
        std::lock_guard lock(state->command->mutex);
        auto& command = *state->command;
        if (command.phase == AdapterAdmissionPhase::NotEstablished ||
            command.phase == AdapterAdmissionPhase::Closed) {
            return gamenet::net::PostResult::Shutdown;
        }
        if (kind == AdapterCommandKind::Shutdown &&
            (command.gracefulAdmitted || command.forceAdmitted)) {
            return gamenet::net::PostResult::Accepted;
        }
        if (kind == AdapterCommandKind::ForceClose &&
            command.forceAdmitted) {
            return gamenet::net::PostResult::Accepted;
        }
        if (command.commands.size() >=
            state->options.maxPendingCommands) {
            ++command.commandQueueRejections;
            return gamenet::net::PostResult::QueueFull;
        }

        const auto previousPhase = command.phase;
        const auto previousCloseInfo = command.firstCloseInfo;
        const bool previousGraceful = command.gracefulAdmitted;
        const bool previousForce = command.forceAdmitted;
        if (kind == AdapterCommandKind::Shutdown) {
            command.gracefulAdmitted = true;
            command.phase = AdapterAdmissionPhase::GracefulSealed;
            if (!command.firstCloseInfo) {
                command.firstCloseInfo = CloseInfo{
                    .reason = gamenet::net::
                        TcpConnectionCloseReason::GracefulShutdown,
                    .nativeError = 0,
                };
            }
        } else {
            command.forceAdmitted = true;
            command.phase = AdapterAdmissionPhase::ForceSealed;
            if (!command.firstCloseInfo) {
                command.firstCloseInfo = CloseInfo{
                    .reason = gamenet::net::
                        TcpConnectionCloseReason::ForcedShutdown,
                    .nativeError = 0,
                };
            }
        }

        AdapterCommand admitted{
            .kind = kind,
            .foreign = foreign,
        };
        if (!foreign && !command.draining) {
            admitted.ownerPostResult = &ownerResult.emplace(
                gamenet::net::PostResult::Accepted);
        }
        try {
            command.commands.push_back(std::move(admitted));
        } catch (...) {
            command.phase = previousPhase;
            command.firstCloseInfo = previousCloseInfo;
            command.gracefulAdmitted = previousGraceful;
            command.forceAdmitted = previousForce;
            ++command.commandQueueRejections;
            return gamenet::net::PostResult::QueueFull;
        }
        if (foreign) {
            signalResult = state->commandSource.signal();
            if (signalResult != gamenet::net::PostResult::Accepted) {
                command.commands.pop_back();
                command.phase = previousPhase;
                command.firstCloseInfo = previousCloseInfo;
                command.gracefulAdmitted = previousGraceful;
                command.forceAdmitted = previousForce;
                return signalResult;
            }
            ++command.foreignLifecycleAdmissions;
        }
        command.maxPendingCommands = (std::max)(
            command.maxPendingCommands,
            command.commands.size());
    } catch (...) {
        return gamenet::net::PostResult::QueueFull;
    }

    if (!foreign) {
        drainAdapterCommands(state, true);
        return ownerResult.value_or(gamenet::net::PostResult::Accepted);
    }
    return gamenet::net::PostResult::Accepted;
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
        state_->command->commands.clear();
        const std::weak_ptr<AdapterState> weak = state_;
        state_->commandSource =
            gamenet::net::detail::EventLoopLifecycleRegistry::attach(
                *ownerLoop,
                [weak] {
                    if (const auto state = weak.lock()) {
                        drainAdapterCommands(state);
                    }
                });
        state_->commandSourceAttached = true;
        state_->observer = facade;
    }

    ~IoUringTcpConnectionAdapterImpl() {
        if (!state_) return;
        if (!state_->loop->isInLoopThread()) std::terminate();
        state_->observer = nullptr;
        const auto admittedCloseInfo = sealCommandState(*state_);
        if (admittedCloseInfo) {
            publishCloseInfo(*state_, *admittedCloseInfo);
        }
        if (state_->commandSourceAttached) {
            gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                *state_->loop,
                state_->commandSource);
            state_->commandSourceAttached = false;
        }
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
        {
            std::lock_guard lock(state_->command->mutex);
            state_->command->phase = AdapterAdmissionPhase::Open;
            state_->command->firstCloseInfo.reset();
            state_->command->reservedOutputBytes = 0;
            state_->command->overloadLatched = false;
            state_->command->gracefulAdmitted = false;
            state_->command->forceAdmitted = false;
        }
        return outcome.result;
    }

    gamenet::net::TcpSendResult trySend(std::string_view payload) {
        return admitAdapterSend(state_, payload);
    }

    gamenet::net::PostResult tryForceClose() {
        return admitAdapterTerminal(state_, AdapterCommandKind::ForceClose);
    }

    gamenet::net::PostResult tryShutdown() {
        return admitAdapterTerminal(state_, AdapterCommandKind::Shutdown);
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
    std::lock_guard lock(state.command->mutex);
    return state.established && state.phase == ClosePhase::Open &&
        state.command->phase == AdapterAdmissionPhase::Open;
}

bool IoUringTcpConnectionAdapter::disconnected() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    return state.phase == ClosePhase::Closed;
}

std::size_t IoUringTcpConnectionAdapter::pendingOutputBytes() const {
    auto& state = *impl_->state();
    assertOwner(state);
    return reservedOutputBytes(state);
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
    std::lock_guard lock(state.command->mutex);
    if (state.phase == ClosePhase::Closed ||
        state.command->phase == AdapterAdmissionPhase::Closed) {
        return ClosePhase::Closed;
    }
    if (state.command->phase == AdapterAdmissionPhase::GracefulSealed ||
        state.command->phase == AdapterAdmissionPhase::ForceSealed) {
        return ClosePhase::Closing;
    }
    return state.phase;
}

std::optional<gamenet::net::TcpConnectionCloseInfo>
IoUringTcpConnectionAdapter::closeInfo() const {
    const auto& state = *impl_->state();
    assertOwner(state);
    if (!state.closeInfo) {
        std::lock_guard lock(state.command->mutex);
        return state.command->firstCloseInfo;
    }
    return state.closeInfo;
}

IoUringTcpConnectionAdapterMetrics
IoUringTcpConnectionAdapter::metrics() const {
    auto& state = *impl_->state();
    assertOwner(state);
    syncCommandMetrics(state);
    return state.metrics;
}

std::shared_future<IoUringTcpConnectionAdapterStopSummary>
IoUringTcpConnectionAdapter::stopFuture() const {
    return impl_->state()->future;
}

}  // namespace gamenet::experimental::io_uring
