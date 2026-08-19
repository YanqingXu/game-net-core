#include "runtime_profiles/MultiIoDedicatedFixedTick.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerOptions.h"
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
                if (index >= 63U) {
                    return std::numeric_limits<std::uint64_t>::max();
                }
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
            "MultiIoDedicatedFixedTick requires a base EventLoop");
    }
    return loop;
}

gamenet::net::EventLoop* requireLogicLoop(
    gamenet::net::EventLoop* baseLoop,
    gamenet::net::EventLoop* logicLoop) {
    if (logicLoop == nullptr || logicLoop == baseLoop) {
        throw std::invalid_argument(
            "MultiIoDedicatedFixedTick requires a distinct logic EventLoop");
    }
    return logicLoop;
}

gamenet::net::RepeatingTimerOptions cadenceOptions(
    const MultiIoDedicatedFixedTickOptions& options) {
    return {
        .mode = gamenet::net::RepeatingTimerMode::FixedRate,
        .maxCatchUpCallbacks = options.maxCatchUpTicks,
    };
}

}  // namespace

struct MultiIoDedicatedFixedTick::ConnectionRoute {
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

struct MultiIoDedicatedFixedTick::CallbackState {
    CallbackState(
        gamenet::net::EventLoop* logicLoopValue,
        MultiIoDedicatedFixedTickHandler handlerValue,
        MultiIoDedicatedFixedTickOptions optionsValue)
        : options(std::move(optionsValue)),
          handler(std::move(handlerValue)),
          queue(options.queueLimits),
          logicLoop(logicLoopValue),
          logicExecutor(logicLoopValue->executor()),
          encoder(options.framing),
          logicStopFuture(logicStopPromise.get_future().share()) {
        if (options.ioThreads < 2 || !handler ||
            options.tickInterval <= Clock::duration::zero() ||
            options.maxCommandsPerTick == 0 ||
            options.maxHandlerWallTime <= Clock::duration::zero() ||
            options.maxTickWallTime <= Clock::duration::zero() ||
            options.framing.maxFramesPerPush == 0 ||
            options.framing.maxFrameBytesPerPush == 0) {
            throw std::invalid_argument(
                "MultiIoDedicatedFixedTick requires positive bounded budgets");
        }
        switch (options.cadence) {
        case FixedTickCadence::FixedRateSkipMissed:
            if (options.maxCatchUpTicks != 0) {
                throw std::invalid_argument(
                    "skip-missed cadence requires zero catch-up ticks");
            }
            break;
        case FixedTickCadence::FixedRateBoundedCatchUp:
            if (options.maxCatchUpTicks == 0) {
                throw std::invalid_argument(
                    "bounded catch-up cadence requires a positive budget");
            }
            break;
        default:
            throw std::invalid_argument("unknown fixed-tick cadence");
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

    void accountLogicDrops(std::size_t commands, std::size_t bytes) noexcept {
        logicStopDroppedCommands.fetch_add(commands, std::memory_order_relaxed);
        logicStopDroppedBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    void markShutdownStarted() noexcept {
        std::lock_guard lock(shutdownMutex);
        if (shutdownStartedAt == Clock::time_point{}) {
            shutdownStartedAt = Clock::now();
        }
    }

    std::uint64_t shutdownElapsedMicros() const noexcept {
        std::lock_guard lock(shutdownMutex);
        if (shutdownStartedAt == Clock::time_point{}) return 0;
        return elapsedMicros(Clock::now() - shutdownStartedAt);
    }

    MultiIoDedicatedFixedTickOptions options;
    MultiIoDedicatedFixedTickHandler handler;
    gamenet::game_logic::GameCommandQueue queue;
    gamenet::net::EventLoop* logicLoop;
    gamenet::net::EventLoopExecutor logicExecutor;
    gamenet::protocol::PacketFramer encoder;

    mutable std::mutex routesMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionRoute>> routes;
    std::unordered_set<std::uint64_t> networkOwners;
    mutable std::mutex shutdownMutex;
    Clock::time_point shutdownStartedAt{};

    gamenet::net::TimerId cadenceTimer;
    Clock::time_point scheduledAt{};
    std::uint64_t ownerCadenceIndex{1};
    std::size_t consecutiveCatchUp{0};
    bool nextTickIsCatchUp{false};

    std::atomic<bool> active{true};
    std::atomic<bool> cadenceSetupPending{false};
    std::atomic<bool> timerRetired{true};
    std::atomic<bool> cadenceStopRequested{false};
    std::atomic<bool> cadenceStopResultPublished{false};
    std::atomic<std::size_t> tickCallbacksInFlight{0};
    std::atomic<bool> logicStopCompleted{false};
    std::atomic<bool> activeTickObserved{false};
    std::atomic<bool> terminalTimerFailure{false};
    std::atomic<gamenet::net::PostResult> cadenceStopPostResult{
        gamenet::net::PostResult::OwnerUnavailable};
    std::atomic<std::uint64_t> nextTransportId{1};
    std::atomic<std::uint64_t> nextGeneration{1};
    std::promise<MultiIoDedicatedFixedTickLogicStopSummary> logicStopPromise;
    std::shared_future<MultiIoDedicatedFixedTickLogicStopSummary> logicStopFuture;

    std::atomic<std::uint64_t> connectionsOpened{0};
    std::atomic<std::uint64_t> connectionsClosed{0};
    std::atomic<std::uint64_t> commandsAccepted{0};
    std::atomic<std::uint64_t> queueFullRejections{0};
    std::atomic<std::uint64_t> payloadTooLargeRejections{0};
    std::atomic<std::uint64_t> stoppedRejections{0};
    std::atomic<std::uint64_t> tickCount{0};
    std::atomic<std::uint64_t> publishedCadenceIndex{0};
    std::atomic<std::uint64_t> emptyTicks{0};
    std::atomic<std::uint64_t> ticksWithWork{0};
    std::atomic<std::uint64_t> catchUpTicks{0};
    std::atomic<std::uint64_t> skippedTicks{0};
    std::atomic<std::size_t> maxConsecutiveCatchUp{0};
    std::atomic<std::uint64_t> tickOverruns{0};
    std::atomic<std::size_t> maxCommandsInTick{0};
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
    std::atomic<std::uint64_t> timerSetupPosts{0};
    std::atomic<std::uint64_t> timerSetupAccepted{0};
    std::atomic<std::uint64_t> timerSetupFailures{0};
    std::atomic<std::uint64_t> timerCancelPosts{0};
    std::atomic<std::uint64_t> timerCancelAccepted{0};
    std::atomic<std::uint64_t> timerCancelQueueFull{0};
    std::atomic<std::uint64_t> timerCancelUnavailable{0};
    std::atomic<std::size_t> logicStopDroppedCommands{0};
    std::atomic<std::size_t> logicStopDroppedBytes{0};
    std::atomic<std::uint64_t> queueAgeMaxUs{0};
    std::atomic<std::uint64_t> shutdownDrainWaitUs{0};
    FixedLatencyHistogram tickJitter;
    FixedLatencyHistogram tickDuration;
    FixedLatencyHistogram queueAge;
};

struct MultiIoDedicatedFixedTick::ConnectionState {
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

MultiIoDedicatedFixedTick::MultiIoDedicatedFixedTick(
    gamenet::net::EventLoop* baseLoop,
    gamenet::net::EventLoop* logicLoop,
    const gamenet::net::InetAddress& listenAddress,
    MultiIoDedicatedFixedTickHandler handler,
    MultiIoDedicatedFixedTickOptions options)
    : baseLoop_(requireBaseLoop(baseLoop)),
      callbackState_(std::make_shared<CallbackState>(
          requireLogicLoop(baseLoop_, logicLoop),
          std::move(handler),
          std::move(options))),
      server_(baseLoop_, listenAddress, "multi_io_dedicated_fixed_tick") {
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

MultiIoDedicatedFixedTick::~MultiIoDedicatedFixedTick() {
    baseLoop_->assertInLoopThread();
    if (!stopped_) stop();
}

void MultiIoDedicatedFixedTick::start() {
    baseLoop_->assertInLoopThread();
    if (started_) return;
    if (stopped_) {
        throw std::logic_error(
            "MultiIoDedicatedFixedTick cannot restart after stop");
    }

    callbackState_->cadenceSetupPending.store(true, std::memory_order_release);
    callbackState_->timerRetired.store(false, std::memory_order_release);
    const auto state = callbackState_;
    const auto posted = callbackState_->logicExecutor.post(
        [state] { installCadenceOnOwner(state); });
    if (posted != gamenet::net::PostResult::Accepted) {
        callbackState_->timerSetupFailures.fetch_add(
            1, std::memory_order_relaxed);
        callbackState_->terminalTimerFailure.store(true, std::memory_order_release);
        callbackState_->cadenceSetupPending.store(false, std::memory_order_release);
        callbackState_->timerRetired.store(true, std::memory_order_release);
        revokeProfile(callbackState_, true);
        if (posted == gamenet::net::PostResult::QueueFull) {
            throw std::overflow_error("fixed-tick cadence setup queue is full");
        }
        throw std::logic_error("fixed-tick cadence owner is unavailable");
    }
    callbackState_->timerSetupPosts.fetch_add(1, std::memory_order_relaxed);

    try {
        server_.start();
        started_ = true;
    } catch (...) {
        revokeProfile(callbackState_, true);
        throw;
    }
}

void MultiIoDedicatedFixedTick::stop() {
    (void)stopGracefully(gamenet::net::TcpServerStopOptions{
        .drainTimeout = std::chrono::milliseconds::zero(),
    });
}

MultiIoDedicatedFixedTickStopHandle
MultiIoDedicatedFixedTick::stopGracefully(
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
MultiIoDedicatedFixedTick::listenAddress() const noexcept {
    return server_.listenAddress();
}

MultiIoDedicatedFixedTickMetrics MultiIoDedicatedFixedTick::metrics() const {
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
        .tickCount = callbackState_->tickCount.load(),
        .cadenceIndex = callbackState_->publishedCadenceIndex.load(),
        .emptyTicks = callbackState_->emptyTicks.load(),
        .ticksWithWork = callbackState_->ticksWithWork.load(),
        .catchUpTicks = callbackState_->catchUpTicks.load(),
        .skippedTicks = callbackState_->skippedTicks.load(),
        .maxConsecutiveCatchUp =
            callbackState_->maxConsecutiveCatchUp.load(),
        .tickOverruns = callbackState_->tickOverruns.load(),
        .maxCommandsInTick = callbackState_->maxCommandsInTick.load(),
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
        .networkOwnerViolations =
            callbackState_->networkOwnerViolations.load(),
        .logicOwnerViolations = callbackState_->logicOwnerViolations.load(),
        .endpointOwnerViolations =
            callbackState_->endpointOwnerViolations.load(),
        .crossDomainHandoffs = callbackState_->crossDomainHandoffs.load(),
        .profileTerminalFailures =
            callbackState_->profileTerminalFailures.load(),
        .timerSetupPosts = callbackState_->timerSetupPosts.load(),
        .timerSetupAccepted = callbackState_->timerSetupAccepted.load(),
        .timerSetupFailures = callbackState_->timerSetupFailures.load(),
        .timerCancelPosts = callbackState_->timerCancelPosts.load(),
        .timerCancelAccepted = callbackState_->timerCancelAccepted.load(),
        .timerCancelQueueFull = callbackState_->timerCancelQueueFull.load(),
        .timerCancelUnavailable =
            callbackState_->timerCancelUnavailable.load(),
        .logicStopDroppedCommands =
            callbackState_->logicStopDroppedCommands.load(),
        .logicStopDroppedBytes = callbackState_->logicStopDroppedBytes.load(),
        .tickJitterP50Us = callbackState_->tickJitter.percentile(500),
        .tickJitterP99Us = callbackState_->tickJitter.percentile(990),
        .tickJitterP999Us = callbackState_->tickJitter.percentile(999),
        .tickDurationP99Us = callbackState_->tickDuration.percentile(990),
        .tickDurationP999Us = callbackState_->tickDuration.percentile(999),
        .queueAgeP99Us = callbackState_->queueAge.percentile(990),
        .queueAgeP999Us = callbackState_->queueAge.percentile(999),
        .queueAgeMaxUs = callbackState_->queueAgeMaxUs.load(),
        .shutdownDrainWaitUs = callbackState_->shutdownDrainWaitUs.load(),
        .queue = callbackState_->queue.snapshot(),
    };
}

void MultiIoDedicatedFixedTick::handleFramerResult(
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

void MultiIoDedicatedFixedTick::submitFrame(
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
        return;
    case gamenet::game_logic::SubmitResult::QueueFull:
        callbackState->queueFullRejections.fetch_add(
            1, std::memory_order_relaxed);
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
        callbackState->stoppedRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::GoingAway);
        return;
    }
}

void MultiIoDedicatedFixedTick::installCadenceOnOwner(
    const std::shared_ptr<CallbackState>& state) {
    if (!state->logicExecutor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (!state->active.load(std::memory_order_acquire)) {
        state->cadenceSetupPending.store(false, std::memory_order_release);
        state->timerRetired.store(true, std::memory_order_release);
        completeLogicStop(state);
        return;
    }

    state->scheduledAt = Clock::now() + state->options.tickInterval;
    state->ownerCadenceIndex = 1;
    state->consecutiveCatchUp = 0;
    state->nextTickIsCatchUp = false;
    const auto scheduled = state->logicLoop->tryRunEvery(
        state->options.tickInterval,
        [state] { runTick(state); },
        cadenceOptions(state->options));
    if (scheduled.result == gamenet::net::PostResult::Accepted) {
        state->cadenceTimer = scheduled.timerId;
        state->timerSetupAccepted.fetch_add(1, std::memory_order_relaxed);
        state->cadenceSetupPending.store(false, std::memory_order_release);
        return;
    }

    state->timerSetupFailures.fetch_add(1, std::memory_order_relaxed);
    state->terminalTimerFailure.store(true, std::memory_order_release);
    state->cadenceSetupPending.store(false, std::memory_order_release);
    state->timerRetired.store(true, std::memory_order_release);
    revokeProfile(state, true);
}

void MultiIoDedicatedFixedTick::runTick(
    const std::shared_ptr<CallbackState>& state) {
    state->tickCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto finish = [&] {
        if (state->tickCallbacksInFlight.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            completeLogicStop(state);
        }
    };

    if (!state->logicExecutor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (!state->active.load(std::memory_order_acquire)) {
        stopCadenceOnOwner(state);
        finish();
        return;
    }

    const auto startedAt = Clock::now();
    const auto tickSequence =
        state->tickCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const FixedTickContext tick{
        .tickSequence = tickSequence,
        .cadenceIndex = state->ownerCadenceIndex,
        .scheduledAt = state->scheduledAt,
        .startedAt = startedAt,
        .catchUp = state->nextTickIsCatchUp,
    };
    state->publishedCadenceIndex.store(
        tick.cadenceIndex, std::memory_order_relaxed);
    state->tickJitter.record(startedAt - tick.scheduledAt);
    if (tick.catchUp) {
        state->catchUpTicks.fetch_add(1, std::memory_order_relaxed);
    }

    auto commands = state->queue.drain(state->options.maxCommandsPerTick);
    updateMaximum(state->maxCommandsInTick, commands.size());
    if (commands.empty()) {
        state->emptyTicks.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->ticksWithWork.fetch_add(1, std::memory_order_relaxed);
    }

    for (std::size_t index = 0; index < commands.size(); ++index) {
        auto& command = commands[index];
        if (!state->active.load(std::memory_order_acquire)) {
            std::size_t bytes = 0;
            for (std::size_t remaining = index;
                 remaining < commands.size();
                 ++remaining) {
                bytes += commands[remaining].payload.size();
            }
            state->accountLogicDrops(commands.size() - index, bytes);
            break;
        }

        const auto age = Clock::now() - command.enqueuedAt;
        state->queueAge.record(age);
        updateMaximum(state->queueAgeMaxUs, elapsedMicros(age));
        auto route = state->findCurrentRoute(
            command.transportId, command.requestId);
        if (!route) {
            state->staleInputs.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        MultiIoDedicatedFixedTickHandlerResult handlerResult;
        const auto handlerStartedAt = Clock::now();
        state->handlerCalls.fetch_add(1, std::memory_order_relaxed);
        try {
            handlerResult = state->handler(
                tick, command.transportId, command.payload);
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
            handlerResult.action == MultiIoDedicatedFixedTickAction::Close;
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
        const auto generation = command.requestId;
        const auto action = handlerResult.action;
        const auto posted = route->ownerExecutor.post(
            [state,
             route,
             generation,
             action,
             encoded = std::move(encoded)]() mutable {
                if (!route->ownerExecutor.isInOwnerThread()) {
                    state->networkOwnerViolations.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (!state->active.load(std::memory_order_acquire) ||
                    !route->isCurrent(generation)) {
                    state->staleOutputs.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (encoded) {
                    const auto sendResult = route->endpoint->send(*encoded);
                    if (sendResult ==
                        gamenet::transport::EndpointResult::Accepted) {
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
                if (action == MultiIoDedicatedFixedTickAction::Close) {
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

    const auto finishedAt = Clock::now();
    const auto duration = finishedAt - startedAt;
    state->tickDuration.record(duration);
    if (duration > state->options.maxTickWallTime) {
        state->tickOverruns.fetch_add(1, std::memory_order_relaxed);
    }

    if (!state->active.load(std::memory_order_acquire)) {
        stopCadenceOnOwner(state);
        finish();
        return;
    }

    auto next = state->scheduledAt + state->options.tickInterval;
    if (next > finishedAt) {
        state->consecutiveCatchUp = 0;
        state->nextTickIsCatchUp = false;
        state->scheduledAt = next;
        ++state->ownerCadenceIndex;
    } else if (
        state->options.cadence ==
            FixedTickCadence::FixedRateBoundedCatchUp &&
        state->consecutiveCatchUp < state->options.maxCatchUpTicks) {
        ++state->consecutiveCatchUp;
        updateMaximum(
            state->maxConsecutiveCatchUp, state->consecutiveCatchUp);
        state->nextTickIsCatchUp = true;
        state->scheduledAt = next;
        ++state->ownerCadenceIndex;
    } else {
        const auto missed = static_cast<std::uint64_t>(
            (finishedAt - next) / state->options.tickInterval) + 1;
        state->skippedTicks.fetch_add(missed, std::memory_order_relaxed);
        state->consecutiveCatchUp = 0;
        state->nextTickIsCatchUp = false;
        state->scheduledAt =
            next + state->options.tickInterval * missed;
        state->ownerCadenceIndex += missed + 1;
    }
    finish();
}

void MultiIoDedicatedFixedTick::stopCadenceOnOwner(
    const std::shared_ptr<CallbackState>& state) {
    if (!state->logicExecutor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    gamenet::net::PostResult result = gamenet::net::PostResult::Accepted;
    if (state->cadenceTimer.valid()) {
        result = state->logicLoop->tryCancel(state->cadenceTimer);
        state->cadenceTimer = {};
    }
    switch (result) {
    case gamenet::net::PostResult::Accepted:
        state->timerCancelAccepted.fetch_add(1, std::memory_order_relaxed);
        break;
    case gamenet::net::PostResult::QueueFull:
        state->timerCancelQueueFull.fetch_add(1, std::memory_order_relaxed);
        break;
    case gamenet::net::PostResult::Shutdown:
    case gamenet::net::PostResult::OwnerUnavailable:
        state->timerCancelUnavailable.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    state->cadenceSetupPending.store(false, std::memory_order_release);
    state->timerRetired.store(true, std::memory_order_release);
    completeLogicStop(state);
}

void MultiIoDedicatedFixedTick::requestCadenceStop(
    const std::shared_ptr<CallbackState>& state) {
    if (state->cadenceStopRequested.exchange(true, std::memory_order_acq_rel)) {
        completeLogicStop(state);
        return;
    }
    if (state->timerRetired.load(std::memory_order_acquire) &&
        !state->cadenceSetupPending.load(std::memory_order_acquire)) {
        state->cadenceStopPostResult.store(
            gamenet::net::PostResult::Accepted, std::memory_order_release);
        state->cadenceStopResultPublished.store(true, std::memory_order_release);
        completeLogicStop(state);
        return;
    }

    const auto posted = state->logicExecutor.post(
        [state] { stopCadenceOnOwner(state); });
    state->cadenceStopPostResult.store(posted, std::memory_order_release);
    if (posted == gamenet::net::PostResult::Accepted) {
        state->timerCancelPosts.fetch_add(1, std::memory_order_relaxed);
    } else if (posted == gamenet::net::PostResult::QueueFull) {
        state->timerCancelQueueFull.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->timerCancelUnavailable.fetch_add(1, std::memory_order_relaxed);
        if (posted == gamenet::net::PostResult::OwnerUnavailable) {
            state->cadenceSetupPending.store(false, std::memory_order_release);
        }
        if (!state->cadenceSetupPending.load(std::memory_order_acquire)) {
            state->timerRetired.store(true, std::memory_order_release);
        }
    }
    state->cadenceStopResultPublished.store(true, std::memory_order_release);
    completeLogicStop(state);
}

void MultiIoDedicatedFixedTick::revokeProfile(
    const std::shared_ptr<CallbackState>& state,
    bool terminalTimerFailure) {
    if (terminalTimerFailure) {
        state->terminalTimerFailure.store(true, std::memory_order_release);
    }
    if (!state->active.exchange(false, std::memory_order_acq_rel)) {
        requestCadenceStop(state);
        completeLogicStop(state);
        return;
    }
    state->markShutdownStarted();
    if (state->tickCallbacksInFlight.load(std::memory_order_acquire) != 0) {
        state->activeTickObserved.store(true, std::memory_order_release);
    }
    if (terminalTimerFailure) {
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
            terminalTimerFailure
                ? gamenet::transport::CloseReason::Overloaded
                : gamenet::transport::CloseReason::GoingAway);
    }
    requestCadenceStop(state);
    completeLogicStop(state);
}

void MultiIoDedicatedFixedTick::completeLogicStop(
    const std::shared_ptr<CallbackState>& state) {
    if (state->active.load(std::memory_order_acquire) ||
        !state->cadenceStopResultPublished.load(std::memory_order_acquire) ||
        state->cadenceSetupPending.load(std::memory_order_acquire) ||
        !state->timerRetired.load(std::memory_order_acquire) ||
        state->tickCallbacksInFlight.load(std::memory_order_acquire) != 0) {
        return;
    }
    bool expected = false;
    if (!state->logicStopCompleted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const auto elapsed = state->shutdownElapsedMicros();
    state->shutdownDrainWaitUs.store(elapsed, std::memory_order_release);
    state->logicStopPromise.set_value({
        .droppedCommands = state->logicStopDroppedCommands.load(),
        .droppedBytes = state->logicStopDroppedBytes.load(),
        .cadenceStopPostResult =
            state->cadenceStopPostResult.load(std::memory_order_acquire),
        .activeTickObserved = state->activeTickObserved.load(),
        .terminalTimerFailure = state->terminalTimerFailure.load(),
        .shutdownDrainWaitUs = elapsed,
    });
}

}  // namespace gamenet::examples
