#include "runtime_profiles/MultiIoQueuedEvent.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/game_logic/GameCommand.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gamenet::examples {
namespace {

using Clock = std::chrono::steady_clock;

template <typename Value>
void updateMaximum(std::atomic<Value>& destination, Value candidate) noexcept {
    auto current = destination.load(std::memory_order_relaxed);
    while (current < candidate &&
           !destination.compare_exchange_weak(
               current,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

std::uint64_t elapsedMicros(Clock::duration duration) noexcept {
    if (duration <= Clock::duration::zero()) return 1;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        duration);
    return static_cast<std::uint64_t>(std::max<std::int64_t>(1, micros.count()));
}

class FixedLatencyHistogram {
public:
    void record(Clock::duration duration) noexcept {
        const auto value = elapsedMicros(duration);
        const auto index = std::min<std::size_t>(
            std::bit_width(value) - 1U, buckets_.size() - 1U);
        buckets_[index].fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t percentile(std::uint64_t numerator) const noexcept {
        std::uint64_t total = 0;
        for (const auto& bucket : buckets_) {
            total += bucket.load(std::memory_order_relaxed);
        }
        if (total == 0) return 0;
        const auto threshold = (total * numerator + 999U) / 1000U;
        std::uint64_t observed = 0;
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            observed += buckets_[index].load(std::memory_order_relaxed);
            if (observed >= threshold) {
                if (index >= 63U) return std::numeric_limits<std::uint64_t>::max();
                return std::uint64_t{1} << index;
            }
        }
        return std::numeric_limits<std::uint64_t>::max();
    }

private:
    std::array<std::atomic<std::uint64_t>, 64> buckets_{};
};

gamenet::net::EventLoop* requireBaseLoop(gamenet::net::EventLoop* loop) {
    if (loop == nullptr) {
        throw std::invalid_argument(
            "MultiIoQueuedEvent requires a base EventLoop");
    }
    return loop;
}

gamenet::net::EventLoop* requireLogicLoop(
    gamenet::net::EventLoop* baseLoop,
    gamenet::net::EventLoop* logicLoop) {
    if (logicLoop == nullptr || logicLoop == baseLoop) {
        throw std::invalid_argument(
            "MultiIoQueuedEvent requires a distinct logic EventLoop");
    }
    return logicLoop;
}

}  // namespace

struct MultiIoQueuedEvent::ConnectionRoute {
    ConnectionRoute(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpointValue,
        std::uint64_t generationValue)
        : endpoint(std::move(endpointValue)),
          ownerExecutor(endpoint->ownerExecutor()),
          generation(generationValue) {}

    bool isCurrent(std::uint64_t expectedGeneration) const noexcept {
        return active.load(std::memory_order_acquire) &&
            generation.load(std::memory_order_acquire) == expectedGeneration;
    }

    bool revoke() noexcept {
        if (!active.exchange(false, std::memory_order_acq_rel)) return false;
        generation.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
    gamenet::net::EventLoopExecutor ownerExecutor;
    std::atomic<std::uint64_t> generation;
    std::atomic<bool> active{true};
};

struct MultiIoQueuedEvent::CallbackState {
    CallbackState(
        gamenet::net::EventLoop* logicLoop,
        MultiIoQueuedHandler handlerValue,
        MultiIoQueuedEventOptions optionsValue)
        : options(std::move(optionsValue)),
          handler(std::move(handlerValue)),
          queue(options.queueLimits),
          logicExecutor(logicLoop->executor()),
          encoder(options.framing),
          logicStopFuture(logicStopPromise.get_future().share()) {
        if (options.ioThreads < 2 || !handler ||
            options.maxCommandsPerDrain == 0 ||
            options.maxHandlerWallTime <= Clock::duration::zero() ||
            options.framing.maxFramesPerPush == 0 ||
            options.framing.maxFrameBytesPerPush == 0) {
            throw std::invalid_argument(
                "MultiIoQueuedEvent requires two I/O owners and positive budgets");
        }
        options.admission.validate();
        options.connectionBackpressure.validate();
    }

    std::shared_ptr<ConnectionRoute> findCurrentRoute(
        gamenet::transport::TransportSessionId id,
        std::uint64_t expectedGeneration) const {
        std::lock_guard lock(routesMutex);
        const auto found = routes.find(id.value);
        if (found == routes.end() ||
            !found->second->isCurrent(expectedGeneration)) {
            return {};
        }
        return found->second;
    }

    void accountLogicDrops(
        std::size_t commands,
        std::size_t bytes) noexcept {
        logicStopDroppedCommands.fetch_add(commands, std::memory_order_relaxed);
        logicStopDroppedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    MultiIoQueuedEventOptions options;
    MultiIoQueuedHandler handler;
    gamenet::game_logic::GameCommandQueue queue;
    gamenet::net::EventLoopExecutor logicExecutor;
    gamenet::protocol::PacketFramer encoder;

    mutable std::mutex routesMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionRoute>> routes;
    std::unordered_set<std::uint64_t> networkOwners;

    std::atomic<bool> active{true};
    std::atomic<bool> drainScheduled{false};
    std::atomic<std::size_t> drainCallbacksInFlight{0};
    std::atomic<bool> logicStopCompleted{false};
    std::atomic<bool> terminalWakeFailure{false};
    std::atomic<std::uint64_t> nextTransportId{1};
    std::atomic<std::uint64_t> nextGeneration{1};
    std::promise<MultiIoQueuedLogicStopSummary> logicStopPromise;
    std::shared_future<MultiIoQueuedLogicStopSummary> logicStopFuture;

    std::atomic<std::uint64_t> connectionsOpened{0};
    std::atomic<std::uint64_t> connectionsClosed{0};
    std::atomic<std::uint64_t> commandsAccepted{0};
    std::atomic<std::uint64_t> queueFullRejections{0};
    std::atomic<std::uint64_t> payloadTooLargeRejections{0};
    std::atomic<std::uint64_t> stoppedRejections{0};
    std::atomic<std::uint64_t> producerWakePosts{0};
    std::atomic<std::uint64_t> producerWakeMerges{0};
    std::atomic<std::uint64_t> producerWakeRejections{0};
    std::atomic<std::uint64_t> drainCallbacks{0};
    std::atomic<std::uint64_t> drainContinuations{0};
    std::atomic<std::uint64_t> drainContinuationRejections{0};
    std::atomic<std::size_t> maxCommandsInDrain{0};
    std::atomic<std::uint64_t> handlerCalls{0};
    std::atomic<std::uint64_t> handlerExceptions{0};
    std::atomic<std::uint64_t> handlerOverruns{0};
    std::atomic<std::uint64_t> staleInputs{0};
    std::atomic<std::uint64_t> staleOutputs{0};
    std::atomic<std::uint64_t> outputPosts{0};
    std::atomic<std::uint64_t> outputPostRejections{0};
    std::atomic<std::uint64_t> outputsAccepted{0};
    std::atomic<std::uint64_t> outputOverloads{0};
    std::atomic<std::uint64_t> protocolFailures{0};
    std::atomic<std::uint64_t> framerContinuationPosts{0};
    std::atomic<std::uint64_t> framerContinuationRejections{0};
    std::atomic<std::uint64_t> networkOwnerViolations{0};
    std::atomic<std::uint64_t> logicOwnerViolations{0};
    std::atomic<std::uint64_t> endpointOwnerViolations{0};
    std::atomic<std::uint64_t> crossDomainHandoffs{0};
    std::atomic<std::uint64_t> profileTerminalFailures{0};
    std::atomic<std::size_t> logicStopDroppedCommands{0};
    std::atomic<std::size_t> logicStopDroppedBytes{0};
    std::atomic<std::uint64_t> queueOldestAgeMaxUs{0};
    FixedLatencyHistogram networkToLogicLatency;
    FixedLatencyHistogram logicToNetworkLatency;
};

struct MultiIoQueuedEvent::ConnectionState {
    ConnectionState(
        const gamenet::protocol::PacketFramerOptions& framing,
        std::shared_ptr<ConnectionRoute> routeValue,
        std::shared_ptr<CallbackState> callbackStateValue)
        : framer(framing),
          route(std::move(routeValue)),
          callbackState(std::move(callbackStateValue)) {}

    gamenet::protocol::PacketFramer framer;
    std::shared_ptr<ConnectionRoute> route;
    std::shared_ptr<CallbackState> callbackState;
    bool closing{false};
    bool continuationQueued{false};
};

MultiIoQueuedEvent::MultiIoQueuedEvent(
    gamenet::net::EventLoop* baseLoop,
    gamenet::net::EventLoop* logicLoop,
    const gamenet::net::InetAddress& listenAddress,
    MultiIoQueuedHandler handler,
    MultiIoQueuedEventOptions options)
    : baseLoop_(requireBaseLoop(baseLoop)),
      callbackState_(std::make_shared<CallbackState>(
          requireLogicLoop(baseLoop_, logicLoop),
          std::move(handler),
          std::move(options))),
      server_(baseLoop_, listenAddress, "multi_io_queued_event") {
    baseLoop_->assertInLoopThread();
    server_.setThreadNum(callbackState_->options.ioThreads);
    server_.setAdmissionOptions(callbackState_->options.admission);
    server_.setConnectionBackpressureOptions(
        callbackState_->options.connectionBackpressure);

    const auto callbackState = callbackState_;
    server_.setConnectionCallback(
        [callbackState](const gamenet::net::TcpConnectionPtr& connection) {
            connection->getLoop()->assertInLoopThread();
            if (connection->connected()) {
                if (!callbackState->active.load(std::memory_order_acquire)) {
                    connection->forceClose();
                    return;
                }
                const auto transportId = gamenet::transport::TransportSessionId{
                    callbackState->nextTransportId.fetch_add(
                        1, std::memory_order_relaxed)};
                const auto generation = callbackState->nextGeneration.fetch_add(
                    1, std::memory_order_relaxed);
                auto endpoint =
                    std::make_shared<gamenet::transport::TcpTransportEndpoint>(
                        transportId, connection);
                auto route = std::make_shared<ConnectionRoute>(
                    std::move(endpoint), generation);
                auto state = std::make_shared<ConnectionState>(
                    callbackState->options.framing, route, callbackState);
                {
                    std::lock_guard lock(callbackState->routesMutex);
                    callbackState->routes.emplace(transportId.value, route);
                    callbackState->networkOwners.insert(route->ownerExecutor.id());
                }
                connection->setContext(std::move(state));
                callbackState->connectionsOpened.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }

            const auto* context =
                std::any_cast<std::shared_ptr<ConnectionState>>(
                    &connection->getContext());
            if (context == nullptr || !*context) return;
            const auto state = *context;
            if (!state->route->ownerExecutor.isInOwnerThread()) {
                callbackState->networkOwnerViolations.fetch_add(
                    1, std::memory_order_relaxed);
            }
            state->closing = true;
            state->continuationQueued = false;
            state->route->revoke();
            {
                std::lock_guard lock(callbackState->routesMutex);
                const auto found = callbackState->routes.find(
                    state->route->endpoint->id().value);
                if (found != callbackState->routes.end() &&
                    found->second == state->route) {
                    callbackState->routes.erase(found);
                }
            }
            connection->setContext(std::any{});
            callbackState->connectionsClosed.fetch_add(
                1, std::memory_order_relaxed);
        });

    server_.setMessageCallback(
        [callbackState](
            const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            connection->getLoop()->assertInLoopThread();
            const auto* context =
                std::any_cast<std::shared_ptr<ConnectionState>>(
                    &connection->getContext());
            if (context == nullptr || !*context) {
                input->retrieveAll();
                return;
            }
            const auto state = *context;
            if (!state->route->ownerExecutor.isInOwnerThread()) {
                callbackState->networkOwnerViolations.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (!callbackState->active.load(std::memory_order_acquire) ||
                state->closing) {
                input->retrieveAll();
                return;
            }
            handleFramerResult(
                state,
                state->framer.push(input->retrieveAllAsString()));
        });
}

MultiIoQueuedEvent::~MultiIoQueuedEvent() {
    baseLoop_->assertInLoopThread();
    if (!stopped_) stop();
}

void MultiIoQueuedEvent::start() {
    baseLoop_->assertInLoopThread();
    if (started_) return;
    if (stopped_) {
        throw std::logic_error("MultiIoQueuedEvent cannot restart after stop");
    }
    server_.start();
    started_ = true;
}

void MultiIoQueuedEvent::stop() {
    (void)stopGracefully(gamenet::net::TcpServerStopOptions{
        .drainTimeout = std::chrono::milliseconds::zero(),
    });
}

MultiIoQueuedStopHandle MultiIoQueuedEvent::stopGracefully(
    gamenet::net::TcpServerStopOptions options) {
    baseLoop_->assertInLoopThread();
    if (!stopped_) {
        revokeProfile(callbackState_, false);
        started_ = false;
        stopped_ = true;
        networkStopFuture_ = server_.stopGracefully(options);
    }
    return {
        .networkStop = networkStopFuture_,
        .logicStop = callbackState_->logicStopFuture,
    };
}

const gamenet::net::InetAddress&
MultiIoQueuedEvent::listenAddress() const noexcept {
    return server_.listenAddress();
}

MultiIoQueuedEventMetrics MultiIoQueuedEvent::metrics() const {
    std::size_t networkOwnerCount = 0;
    {
        std::lock_guard lock(callbackState_->routesMutex);
        networkOwnerCount = callbackState_->networkOwners.size();
    }
    return {
        .connectionsOpened = callbackState_->connectionsOpened.load(),
        .connectionsClosed = callbackState_->connectionsClosed.load(),
        .networkOwnerCount = networkOwnerCount,
        .commandsAccepted = callbackState_->commandsAccepted.load(),
        .queueFullRejections = callbackState_->queueFullRejections.load(),
        .payloadTooLargeRejections =
            callbackState_->payloadTooLargeRejections.load(),
        .stoppedRejections = callbackState_->stoppedRejections.load(),
        .producerWakePosts = callbackState_->producerWakePosts.load(),
        .producerWakeMerges = callbackState_->producerWakeMerges.load(),
        .producerWakeRejections = callbackState_->producerWakeRejections.load(),
        .drainCallbacks = callbackState_->drainCallbacks.load(),
        .drainContinuations = callbackState_->drainContinuations.load(),
        .drainContinuationRejections =
            callbackState_->drainContinuationRejections.load(),
        .maxCommandsInDrain = callbackState_->maxCommandsInDrain.load(),
        .handlerCalls = callbackState_->handlerCalls.load(),
        .handlerExceptions = callbackState_->handlerExceptions.load(),
        .handlerOverruns = callbackState_->handlerOverruns.load(),
        .staleInputs = callbackState_->staleInputs.load(),
        .staleOutputs = callbackState_->staleOutputs.load(),
        .outputPosts = callbackState_->outputPosts.load(),
        .outputPostRejections = callbackState_->outputPostRejections.load(),
        .outputsAccepted = callbackState_->outputsAccepted.load(),
        .outputOverloads = callbackState_->outputOverloads.load(),
        .protocolFailures = callbackState_->protocolFailures.load(),
        .framerContinuationPosts =
            callbackState_->framerContinuationPosts.load(),
        .framerContinuationRejections =
            callbackState_->framerContinuationRejections.load(),
        .networkOwnerViolations = callbackState_->networkOwnerViolations.load(),
        .logicOwnerViolations = callbackState_->logicOwnerViolations.load(),
        .endpointOwnerViolations =
            callbackState_->endpointOwnerViolations.load(),
        .crossDomainHandoffs = callbackState_->crossDomainHandoffs.load(),
        .profileTerminalFailures =
            callbackState_->profileTerminalFailures.load(),
        .logicStopDroppedCommands =
            callbackState_->logicStopDroppedCommands.load(),
        .logicStopDroppedBytes = callbackState_->logicStopDroppedBytes.load(),
        .networkToLogicP99Us =
            callbackState_->networkToLogicLatency.percentile(990),
        .networkToLogicP999Us =
            callbackState_->networkToLogicLatency.percentile(999),
        .logicToNetworkP99Us =
            callbackState_->logicToNetworkLatency.percentile(990),
        .logicToNetworkP999Us =
            callbackState_->logicToNetworkLatency.percentile(999),
        .queueOldestAgeMaxUs = callbackState_->queueOldestAgeMaxUs.load(),
        .queue = callbackState_->queue.snapshot(),
    };
}

void MultiIoQueuedEvent::handleFramerResult(
    const std::shared_ptr<ConnectionState>& state,
    gamenet::protocol::FrameResult result) {
    const auto callbackState = state->callbackState;
    if (state->closing ||
        !callbackState->active.load(std::memory_order_acquire)) {
        return;
    }

    const auto close = [state](gamenet::transport::CloseReason reason) {
        if (state->closing) return;
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(reason);
    };

    if (result.status == gamenet::protocol::FrameStatus::FrameTooLarge ||
        result.status == gamenet::protocol::FrameStatus::BufferLimitExceeded ||
        result.status == gamenet::protocol::FrameStatus::Faulted) {
        callbackState->protocolFailures.fetch_add(1, std::memory_order_relaxed);
        close(gamenet::transport::CloseReason::ProtocolError);
        return;
    }

    for (auto& frame : result.frames) {
        if (state->closing ||
            !callbackState->active.load(std::memory_order_acquire)) {
            return;
        }
        submitFrame(state, std::move(frame));
    }

    if (!result.needsContinuation || state->continuationQueued ||
        state->closing ||
        !callbackState->active.load(std::memory_order_acquire)) {
        return;
    }

    state->continuationQueued = true;
    const auto posted = state->route->ownerExecutor.post([state] {
        state->continuationQueued = false;
        if (state->closing ||
            !state->callbackState->active.load(std::memory_order_acquire)) {
            return;
        }
        handleFramerResult(state, state->framer.push({}));
    });
    if (posted == gamenet::net::PostResult::Accepted) {
        callbackState->framerContinuationPosts.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    state->continuationQueued = false;
    callbackState->framerContinuationRejections.fetch_add(
        1, std::memory_order_relaxed);
    close(
        posted == gamenet::net::PostResult::QueueFull
            ? gamenet::transport::CloseReason::Overloaded
            : gamenet::transport::CloseReason::GoingAway);
}

void MultiIoQueuedEvent::submitFrame(
    const std::shared_ptr<ConnectionState>& state,
    std::string payload) {
    const auto callbackState = state->callbackState;
    const auto generation = state->route->generation.load(
        std::memory_order_acquire);
    gamenet::game_logic::GameCommand command{
        .transportId = state->route->endpoint->id(),
        .requestId = generation,
        .payload = std::move(payload),
        .enqueuedAt = Clock::now(),
    };
    const auto result = callbackState->queue.submit(std::move(command));
    switch (result) {
    case gamenet::game_logic::SubmitResult::Accepted:
        callbackState->commandsAccepted.fetch_add(1, std::memory_order_relaxed);
        callbackState->crossDomainHandoffs.fetch_add(
            1, std::memory_order_relaxed);
        scheduleProducerDrain(callbackState);
        return;
    case gamenet::game_logic::SubmitResult::QueueFull:
        callbackState->queueFullRejections.fetch_add(1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::Overloaded);
        return;
    case gamenet::game_logic::SubmitResult::PayloadTooLarge:
        callbackState->payloadTooLargeRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::ProtocolError);
        return;
    case gamenet::game_logic::SubmitResult::Stopped:
    case gamenet::game_logic::SubmitResult::StaleBinding:
        callbackState->stoppedRejections.fetch_add(1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::GoingAway);
        return;
    }
}

void MultiIoQueuedEvent::scheduleProducerDrain(
    const std::shared_ptr<CallbackState>& state) {
    if (state->drainScheduled.exchange(true, std::memory_order_acq_rel)) {
        state->producerWakeMerges.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto posted = state->logicExecutor.post([state] { drainQueue(state); });
    if (posted == gamenet::net::PostResult::Accepted) {
        state->producerWakePosts.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    state->producerWakeRejections.fetch_add(1, std::memory_order_relaxed);
    state->drainScheduled.store(false, std::memory_order_release);
    revokeProfile(state, true);
}

void MultiIoQueuedEvent::drainQueue(
    const std::shared_ptr<CallbackState>& state) {
    state->drainCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto finish = [&] {
        if (state->drainCallbacksInFlight.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            completeLogicStop(state);
        }
    };

    if (!state->logicExecutor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    state->drainCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!state->active.load(std::memory_order_acquire)) {
        state->drainScheduled.store(false, std::memory_order_release);
        finish();
        return;
    }

    auto commands = state->queue.drain(state->options.maxCommandsPerDrain);
    updateMaximum(state->maxCommandsInDrain, commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index) {
        auto& command = commands[index];
        if (!state->active.load(std::memory_order_acquire)) {
            std::size_t bytes = 0;
            for (std::size_t remaining = index; remaining < commands.size(); ++remaining) {
                bytes += commands[remaining].payload.size();
            }
            state->accountLogicDrops(commands.size() - index, bytes);
            break;
        }

        const auto queueAge = Clock::now() - command.enqueuedAt;
        state->networkToLogicLatency.record(queueAge);
        updateMaximum(state->queueOldestAgeMaxUs, elapsedMicros(queueAge));
        auto route = state->findCurrentRoute(
            command.transportId, command.requestId);
        if (!route) {
            state->staleInputs.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        MultiIoQueuedHandlerResult handlerResult;
        const auto handlerStartedAt = Clock::now();
        state->handlerCalls.fetch_add(1, std::memory_order_relaxed);
        try {
            handlerResult = state->handler(command.transportId, command.payload);
        } catch (...) {
            state->handlerExceptions.fetch_add(1, std::memory_order_relaxed);
            route->revoke();
            (void)route->endpoint->requestClose(
                gamenet::transport::CloseReason::GoingAway);
            continue;
        }
        if (Clock::now() - handlerStartedAt >
            state->options.maxHandlerWallTime) {
            state->handlerOverruns.fetch_add(1, std::memory_order_relaxed);
            route->revoke();
            (void)route->endpoint->requestClose(
                gamenet::transport::CloseReason::Overloaded);
            continue;
        }

        const bool hasOwnerOutput = handlerResult.reply.has_value() ||
            handlerResult.action == MultiIoQueuedAction::Close;
        if (!hasOwnerOutput) continue;
        if (!state->active.load(std::memory_order_acquire) ||
            !route->isCurrent(command.requestId)) {
            state->staleOutputs.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::optional<std::string> encoded;
        if (handlerResult.reply) {
            encoded = state->encoder.encode(*handlerResult.reply);
            if (!encoded) {
                state->protocolFailures.fetch_add(1, std::memory_order_relaxed);
                route->revoke();
                (void)route->endpoint->requestClose(
                    gamenet::transport::CloseReason::ProtocolError);
                continue;
            }
        }
        const auto logicFinishedAt = Clock::now();
        const auto generation = command.requestId;
        const auto action = handlerResult.action;
        const auto posted = route->ownerExecutor.post(
            [state,
             route,
             generation,
             action,
             encoded = std::move(encoded),
             logicFinishedAt]() mutable {
                if (!route->ownerExecutor.isInOwnerThread()) {
                    state->networkOwnerViolations.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (!state->active.load(std::memory_order_acquire) ||
                    !route->isCurrent(generation)) {
                    state->staleOutputs.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                state->logicToNetworkLatency.record(
                    Clock::now() - logicFinishedAt);
                if (encoded) {
                    const auto sendResult = route->endpoint->send(*encoded);
                    if (sendResult == gamenet::transport::EndpointResult::Accepted) {
                        state->outputsAccepted.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        if (sendResult ==
                            gamenet::transport::EndpointResult::WrongThread) {
                            state->endpointOwnerViolations.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        if (sendResult ==
                            gamenet::transport::EndpointResult::Overloaded) {
                            state->outputOverloads.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        route->revoke();
                        (void)route->endpoint->close(
                            sendResult ==
                                    gamenet::transport::EndpointResult::Overloaded
                                ? gamenet::transport::CloseReason::Overloaded
                                : gamenet::transport::CloseReason::GoingAway);
                        return;
                    }
                }
                if (action == MultiIoQueuedAction::Close) {
                    route->revoke();
                    (void)route->endpoint->close(
                        gamenet::transport::CloseReason::Normal);
                }
            });
        if (posted == gamenet::net::PostResult::Accepted) {
            state->outputPosts.fetch_add(1, std::memory_order_relaxed);
            state->crossDomainHandoffs.fetch_add(1, std::memory_order_relaxed);
        } else {
            state->outputPostRejections.fetch_add(1, std::memory_order_relaxed);
            route->revoke();
            (void)route->endpoint->requestClose(
                posted == gamenet::net::PostResult::QueueFull
                    ? gamenet::transport::CloseReason::Overloaded
                    : gamenet::transport::CloseReason::GoingAway);
        }
    }

    if (!state->active.load(std::memory_order_acquire)) {
        state->drainScheduled.store(false, std::memory_order_release);
        finish();
        return;
    }
    if (state->queue.snapshot().depth != 0) {
        const auto posted = state->logicExecutor.post([state] { drainQueue(state); });
        if (posted == gamenet::net::PostResult::Accepted) {
            state->drainContinuations.fetch_add(1, std::memory_order_relaxed);
            finish();
            return;
        }
        state->drainContinuationRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->drainScheduled.store(false, std::memory_order_release);
        revokeProfile(state, true);
        finish();
        return;
    }

    state->drainScheduled.store(false, std::memory_order_release);
    if (state->active.load(std::memory_order_acquire) &&
        state->queue.snapshot().depth != 0 &&
        !state->drainScheduled.exchange(true, std::memory_order_acq_rel)) {
        const auto posted = state->logicExecutor.post([state] { drainQueue(state); });
        if (posted == gamenet::net::PostResult::Accepted) {
            state->drainContinuations.fetch_add(1, std::memory_order_relaxed);
        } else {
            state->drainContinuationRejections.fetch_add(
                1, std::memory_order_relaxed);
            state->drainScheduled.store(false, std::memory_order_release);
            revokeProfile(state, true);
        }
    }
    finish();
}

void MultiIoQueuedEvent::revokeProfile(
    const std::shared_ptr<CallbackState>& state,
    bool terminalWakeFailure) {
    if (terminalWakeFailure) {
        state->terminalWakeFailure.store(true, std::memory_order_release);
    }
    if (!state->active.exchange(false, std::memory_order_acq_rel)) {
        completeLogicStop(state);
        return;
    }
    if (terminalWakeFailure) {
        state->profileTerminalFailures.fetch_add(1, std::memory_order_relaxed);
    }
    const auto discarded = state->queue.closeAndDiscard();
    state->accountLogicDrops(
        discarded.droppedCommands, discarded.droppedBytes);

    std::vector<std::shared_ptr<ConnectionRoute>> routes;
    {
        std::lock_guard lock(state->routesMutex);
        routes.reserve(state->routes.size());
        for (auto& [id, route] : state->routes) {
            (void)id;
            routes.push_back(route);
        }
        state->routes.clear();
    }
    for (const auto& route : routes) {
        route->revoke();
        (void)route->endpoint->requestClose(
            terminalWakeFailure
                ? gamenet::transport::CloseReason::Overloaded
                : gamenet::transport::CloseReason::GoingAway);
    }
    completeLogicStop(state);
}

void MultiIoQueuedEvent::completeLogicStop(
    const std::shared_ptr<CallbackState>& state) {
    if (state->active.load(std::memory_order_acquire) ||
        state->drainScheduled.load(std::memory_order_acquire) ||
        state->drainCallbacksInFlight.load(std::memory_order_acquire) != 0) {
        return;
    }
    bool expected = false;
    if (!state->logicStopCompleted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    state->logicStopPromise.set_value({
        .droppedCommands = state->logicStopDroppedCommands.load(),
        .droppedBytes = state->logicStopDroppedBytes.load(),
        .terminalWakeFailure = state->terminalWakeFailure.load(),
    });
}

}  // namespace gamenet::examples
