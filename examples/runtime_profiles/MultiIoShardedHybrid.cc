#include "runtime_profiles/MultiIoShardedHybrid.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerOptions.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(duration);
    return static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, micros.count()));
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
            "MultiIoShardedHybrid requires a base EventLoop");
    }
    return loop;
}

std::uint64_t stableHash(const LogicShardKey& key) noexcept {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto result = offset;
    result ^= static_cast<std::uint8_t>(key.kind);
    result *= prime;
    for (const auto byte : key.value) {
        result ^= static_cast<unsigned char>(byte);
        result *= prime;
    }
    return result;
}

gamenet::net::EventLoopSelectionPolicy connectionPolicy(
    ConnectionPlacementPolicy policy) {
    switch (policy) {
    case ConnectionPlacementPolicy::RoundRobin:
        return gamenet::net::EventLoopSelectionPolicy::RoundRobin;
    case ConnectionPlacementPolicy::LeastConnections:
        return gamenet::net::EventLoopSelectionPolicy::LeastConnections;
    case ConnectionPlacementPolicy::QueueLag:
        return gamenet::net::EventLoopSelectionPolicy::QueueLag;
    case ConnectionPlacementPolicy::ConsistentHash:
        return gamenet::net::EventLoopSelectionPolicy::ConsistentHash;
    }
    throw std::invalid_argument("invalid connection placement policy");
}

bool validKeyKind(LogicShardKeyKind kind) noexcept {
    switch (kind) {
    case LogicShardKeyKind::Player:
    case LogicShardKeyKind::Room:
    case LogicShardKeyKind::Scene:
        return true;
    }
    return false;
}

bool validLane(HybridDispatchLane lane) noexcept {
    switch (lane) {
    case HybridDispatchLane::EventDriven:
    case HybridDispatchLane::FixedTick:
        return true;
    }
    return false;
}

}  // namespace

struct MultiIoShardedHybrid::ShardedHybridCommand {
    gamenet::transport::TransportSessionId transportId{};
    std::uint64_t generation{};
    std::uint64_t connectionOwnerExecutorId{};
    LogicShardKey key;
    HybridDispatchLane lane{HybridDispatchLane::EventDriven};
    std::string payload;
    Clock::time_point enqueuedAt{};
    std::uint64_t sequence{};

    std::size_t queuedBytes() const noexcept {
        return key.value.size() + payload.size();
    }
};

class MultiIoShardedHybrid::CellQueue {
public:
    enum class SubmitResult {
        Accepted,
        QueueFull,
        PayloadTooLarge,
        KeyInvalid,
        Stopped,
    };

    struct SubmitOutcome {
        SubmitResult result{SubmitResult::Stopped};
        std::uint64_t sequence{};
    };

    struct DrainResult {
        std::vector<ShardedHybridCommand> commands;
        bool blockedByFixed{};
    };

    struct Snapshot {
        std::size_t depth{};
        std::size_t queuedBytes{};
        std::size_t depthHighWatermark{};
        std::size_t bytesHighWatermark{};
        std::uint64_t accepted{};
        std::uint64_t eventAccepted{};
        std::uint64_t fixedAccepted{};
        std::uint64_t droppedOnStop{};
        std::uint64_t droppedBytesOnStop{};
        std::uint64_t maximumSequence{};
        bool accepting{};
    };

    struct DiscardSummary {
        std::size_t commands{};
        std::size_t bytes{};
    };

    explicit CellQueue(const MultiIoShardedHybridOptions& options)
        : maxCommandsPerCell_(options.maxCommandsPerCell),
          maxQueuedBytesPerCell_(options.maxQueuedBytesPerCell),
          maxPayloadBytes_(options.maxPayloadBytes),
          maxShardKeyBytes_(options.maxShardKeyBytes) {}

    SubmitOutcome submit(ShardedHybridCommand command) {
        std::lock_guard lock(mutex_);
        if (!accepting_) return {SubmitResult::Stopped, 0};
        if (command.payload.size() > maxPayloadBytes_) {
            return {SubmitResult::PayloadTooLarge, 0};
        }
        if (command.key.value.empty() ||
            command.key.value.size() > maxShardKeyBytes_ ||
            !validKeyKind(command.key.kind)) {
            return {SubmitResult::KeyInvalid, 0};
        }
        if (command.key.value.size() >
            maxQueuedBytesPerCell_ - command.payload.size()) {
            return {SubmitResult::QueueFull, 0};
        }
        const auto bytes = command.queuedBytes();
        if (queue_.size() >= maxCommandsPerCell_ ||
            bytes > maxQueuedBytesPerCell_ - queuedBytes_) {
            return {SubmitResult::QueueFull, 0};
        }
        command.sequence = ++nextSequence_;
        const auto sequence = command.sequence;
        queuedBytes_ += bytes;
        queue_.push_back(std::move(command));
        ++accepted_;
        if (queue_.back().lane == HybridDispatchLane::EventDriven) {
            ++eventAccepted_;
        } else {
            ++fixedAccepted_;
        }
        depthHighWatermark_ = std::max(depthHighWatermark_, queue_.size());
        bytesHighWatermark_ = std::max(bytesHighWatermark_, queuedBytes_);
        return {SubmitResult::Accepted, sequence};
    }

    DrainResult drainEventPrefix(std::size_t maximum) {
        std::lock_guard lock(mutex_);
        DrainResult result;
        if (queue_.empty()) return result;
        if (queue_.front().lane == HybridDispatchLane::FixedTick) {
            result.blockedByFixed = std::any_of(
                std::next(queue_.begin()),
                queue_.end(),
                [](const auto& command) {
                    return command.lane == HybridDispatchLane::EventDriven;
                });
            return result;
        }
        result.commands.reserve(std::min(maximum, queue_.size()));
        while (result.commands.size() < maximum && !queue_.empty() &&
               queue_.front().lane == HybridDispatchLane::EventDriven) {
            queuedBytes_ -= queue_.front().queuedBytes();
            result.commands.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        if (!queue_.empty() &&
            queue_.front().lane == HybridDispatchLane::FixedTick) {
            result.blockedByFixed = std::any_of(
                std::next(queue_.begin()),
                queue_.end(),
                [](const auto& command) {
                    return command.lane == HybridDispatchLane::EventDriven;
                });
        }
        return result;
    }

    std::vector<ShardedHybridCommand> drainTickPrefix(std::size_t maximum) {
        std::lock_guard lock(mutex_);
        std::vector<ShardedHybridCommand> result;
        const auto count = std::min(maximum, queue_.size());
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            queuedBytes_ -= queue_.front().queuedBytes();
            result.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return result;
    }

    bool hasEventHead() const {
        std::lock_guard lock(mutex_);
        return !queue_.empty() &&
            queue_.front().lane == HybridDispatchLane::EventDriven;
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {
            .depth = queue_.size(),
            .queuedBytes = queuedBytes_,
            .depthHighWatermark = depthHighWatermark_,
            .bytesHighWatermark = bytesHighWatermark_,
            .accepted = accepted_,
            .eventAccepted = eventAccepted_,
            .fixedAccepted = fixedAccepted_,
            .droppedOnStop = droppedOnStop_,
            .droppedBytesOnStop = droppedBytesOnStop_,
            .maximumSequence = nextSequence_,
            .accepting = accepting_,
        };
    }

    DiscardSummary closeAndDiscard() {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        const DiscardSummary summary{
            .commands = queue_.size(),
            .bytes = queuedBytes_,
        };
        droppedOnStop_ += summary.commands;
        droppedBytesOnStop_ += summary.bytes;
        queue_.clear();
        queuedBytes_ = 0;
        return summary;
    }

private:
    const std::size_t maxCommandsPerCell_;
    const std::size_t maxQueuedBytesPerCell_;
    const std::size_t maxPayloadBytes_;
    const std::size_t maxShardKeyBytes_;
    mutable std::mutex mutex_;
    std::deque<ShardedHybridCommand> queue_;
    std::size_t queuedBytes_{};
    std::size_t depthHighWatermark_{};
    std::size_t bytesHighWatermark_{};
    std::uint64_t accepted_{};
    std::uint64_t eventAccepted_{};
    std::uint64_t fixedAccepted_{};
    std::uint64_t droppedOnStop_{};
    std::uint64_t droppedBytesOnStop_{};
    std::uint64_t nextSequence_{};
    bool accepting_{true};
};

struct MultiIoShardedHybrid::ConnectionRoute {
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

struct MultiIoShardedHybrid::CellState {
    CellState(
        std::size_t indexValue,
        gamenet::net::EventLoop* loopValue,
        const MultiIoShardedHybridOptions& options)
        : index(indexValue),
          logicLoop(loopValue),
          executor(loopValue->executor()),
          queue(options) {}

    std::size_t index;
    gamenet::net::EventLoop* logicLoop;
    gamenet::net::EventLoopExecutor executor;
    CellQueue queue;
    gamenet::net::TimerId timer;
    std::uint64_t ownerLastHandledSequence{};

    std::atomic<bool> cadenceSetupPending{false};
    std::atomic<bool> timerRetired{true};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> eventDrainScheduled{false};
    std::atomic<std::size_t> callbacksInFlight{0};
    std::atomic<std::uint64_t> eventDrainCallbacks{0};
    std::atomic<std::uint64_t> eventDrainContinuations{0};
    std::atomic<std::uint64_t> fixedTicks{0};
    std::atomic<std::uint64_t> emptyTicks{0};
    std::atomic<std::uint64_t> fixedHeadDeferrals{0};
    std::atomic<std::uint64_t> handlerCalls{0};
    std::atomic<std::uint64_t> committedDrops{0};
    std::atomic<std::uint64_t> committedDropBytes{0};
    std::atomic<std::uint64_t> queueAgeMaxUs{0};
    FixedLatencyHistogram queueAge;
};

struct MultiIoShardedHybrid::CallbackState {
    CallbackState(
        gamenet::net::EventLoop* baseLoop,
        std::vector<gamenet::net::EventLoop*> logicLoops,
        MultiIoShardedHybridRouter routerValue,
        MultiIoShardedHybridHandler handlerValue,
        MultiIoShardedHybridOptions optionsValue)
        : options(std::move(optionsValue)),
          router(std::move(routerValue)),
          handler(std::move(handlerValue)),
          encoder(options.framing),
          logicStopFuture(logicStopPromise.get_future().share()) {
        if (options.ioThreads < 2 || logicLoops.size() < 2 || !router ||
            !handler || options.maxCommandsPerCell == 0 ||
            options.maxQueuedBytesPerCell == 0 ||
            options.maxPayloadBytes == 0 || options.maxShardKeyBytes == 0 ||
            options.maxPayloadBytes > options.maxQueuedBytesPerCell ||
            options.maxShardKeyBytes > options.maxQueuedBytesPerCell ||
            options.maxCommandsPerEventDrain == 0 ||
            options.maxCommandsPerTick == 0 ||
            options.tickInterval <= Clock::duration::zero() ||
            options.maxRouterWallTime <= Clock::duration::zero() ||
            options.maxHandlerWallTime <= Clock::duration::zero() ||
            options.maxTurnWallTime <= Clock::duration::zero() ||
            options.framing.maxFramesPerPush == 0 ||
            options.framing.maxFrameBytesPerPush == 0) {
            throw std::invalid_argument(
                "MultiIoShardedHybrid requires finite positive topology and budgets");
        }
        if (options.logicShardPolicy != LogicShardPolicy::StableHash) {
            throw std::invalid_argument("invalid logic shard policy");
        }
        (void)connectionPolicy(options.connectionPlacement);
        options.admission.validate();
        options.connectionBackpressure.validate();

        std::unordered_set<gamenet::net::EventLoop*> uniqueLoops;
        cells.reserve(logicLoops.size());
        for (std::size_t index = 0; index < logicLoops.size(); ++index) {
            auto* loop = logicLoops[index];
            if (loop == nullptr || loop == baseLoop ||
                !uniqueLoops.insert(loop).second) {
                throw std::invalid_argument(
                    "MultiIoShardedHybrid requires distinct external logic loops");
            }
            cells.push_back(std::make_shared<CellState>(index, loop, options));
        }
    }

    std::size_t selectLogicShard(const LogicShardKey& key) const noexcept {
        return static_cast<std::size_t>(stableHash(key) % cells.size());
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

    void accountDrops(
        const std::shared_ptr<CellState>& cell,
        std::size_t commands,
        std::size_t bytes) noexcept {
        logicStopDroppedCommands.fetch_add(commands, std::memory_order_relaxed);
        logicStopDroppedBytes.fetch_add(bytes, std::memory_order_relaxed);
        cell->committedDrops.fetch_add(commands, std::memory_order_relaxed);
        cell->committedDropBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    void markShutdownStarted() {
        std::lock_guard lock(shutdownMutex);
        if (!shutdownStarted) {
            shutdownStarted = true;
            shutdownStartedAt = Clock::now();
        }
    }

    std::uint64_t shutdownElapsedMicros() const {
        std::lock_guard lock(shutdownMutex);
        if (!shutdownStarted) return 1;
        return elapsedMicros(Clock::now() - shutdownStartedAt);
    }

    MultiIoShardedHybridOptions options;
    MultiIoShardedHybridRouter router;
    MultiIoShardedHybridHandler handler;
    gamenet::protocol::PacketFramer encoder;
    std::vector<std::shared_ptr<CellState>> cells;

    mutable std::mutex routesMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionRoute>> routes;
    std::unordered_set<std::uint64_t> networkOwners;

    std::atomic<bool> active{true};
    std::atomic<bool> logicStopCompleted{false};
    std::atomic<bool> activeCallbacksObserved{false};
    std::atomic<bool> terminalCellFailure{false};
    std::atomic<std::uint64_t> nextTransportId{1};
    std::atomic<std::uint64_t> nextGeneration{1};
    std::promise<MultiIoShardedHybridLogicStopSummary> logicStopPromise;
    std::shared_future<MultiIoShardedHybridLogicStopSummary> logicStopFuture;

    mutable std::mutex shutdownMutex;
    bool shutdownStarted{};
    Clock::time_point shutdownStartedAt{};

    std::atomic<std::uint64_t> connectionsOpened{0};
    std::atomic<std::uint64_t> connectionsClosed{0};
    std::atomic<std::uint64_t> stableHashSelections{0};
    std::atomic<std::uint64_t> commandsAccepted{0};
    std::atomic<std::uint64_t> eventCommandsAccepted{0};
    std::atomic<std::uint64_t> fixedCommandsAccepted{0};
    std::atomic<std::uint64_t> queueFullRejections{0};
    std::atomic<std::uint64_t> payloadTooLargeRejections{0};
    std::atomic<std::uint64_t> shardKeyRejections{0};
    std::atomic<std::uint64_t> stoppedRejections{0};
    std::atomic<std::uint64_t> routerCalls{0};
    std::atomic<std::uint64_t> routerExceptions{0};
    std::atomic<std::uint64_t> routerOverruns{0};
    std::atomic<std::uint64_t> eventDrainPosts{0};
    std::atomic<std::uint64_t> eventDrainMerges{0};
    std::atomic<std::uint64_t> eventDrainPostRejections{0};
    std::atomic<std::uint64_t> eventDrainContinuations{0};
    std::atomic<std::uint64_t> fixedHeadDeferrals{0};
    std::atomic<std::uint64_t> fixedTicks{0};
    std::atomic<std::uint64_t> handlerCalls{0};
    std::atomic<std::uint64_t> eventHandlerCalls{0};
    std::atomic<std::uint64_t> fixedHandlerCalls{0};
    std::atomic<std::uint64_t> handlerExceptions{0};
    std::atomic<std::uint64_t> handlerOverruns{0};
    std::atomic<std::uint64_t> turnOverruns{0};
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
    std::atomic<std::uint64_t> connectionOwnerMigrations{0};
    std::atomic<std::uint64_t> orderViolations{0};
    std::atomic<std::uint64_t> crossDomainHandoffs{0};
    std::atomic<std::uint64_t> profileTerminalFailures{0};
    std::atomic<std::uint64_t> timerSetupFailures{0};
    std::atomic<std::uint64_t> timerCancelFailures{0};
    std::atomic<std::size_t> logicStopDroppedCommands{0};
    std::atomic<std::size_t> logicStopDroppedBytes{0};
    std::atomic<std::uint64_t> queueAgeMaxUs{0};
    std::atomic<std::uint64_t> shutdownDrainWaitUs{0};
    FixedLatencyHistogram queueAge;
};

struct MultiIoShardedHybrid::ConnectionState {
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

MultiIoShardedHybrid::MultiIoShardedHybrid(
    gamenet::net::EventLoop* baseLoop,
    std::vector<gamenet::net::EventLoop*> logicLoops,
    const gamenet::net::InetAddress& listenAddress,
    MultiIoShardedHybridRouter router,
    MultiIoShardedHybridHandler handler,
    MultiIoShardedHybridOptions options)
    : baseLoop_(requireBaseLoop(baseLoop)),
      callbackState_(std::make_shared<CallbackState>(
          baseLoop_,
          std::move(logicLoops),
          std::move(router),
          std::move(handler),
          std::move(options))),
      server_(baseLoop_, listenAddress, "multi_io_sharded_hybrid") {
    baseLoop_->assertInLoopThread();
    server_.setThreadNum(callbackState_->options.ioThreads);
    server_.setLoopSelectionPolicy(
        connectionPolicy(callbackState_->options.connectionPlacement));
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
                    callbackState->networkOwners.insert(
                        route->ownerExecutor.id());
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

MultiIoShardedHybrid::~MultiIoShardedHybrid() {
    baseLoop_->assertInLoopThread();
    if (!stopped_) stop();
}

void MultiIoShardedHybrid::start() {
    baseLoop_->assertInLoopThread();
    if (started_) return;
    if (stopped_) {
        throw std::logic_error("MultiIoShardedHybrid cannot restart after stop");
    }

    for (const auto& cell : callbackState_->cells) {
        cell->cadenceSetupPending.store(true, std::memory_order_release);
        cell->timerRetired.store(false, std::memory_order_release);
        const auto state = callbackState_;
        const auto posted = cell->executor.post(
            [state, cell] { installCellCadence(state, cell); });
        if (posted != gamenet::net::PostResult::Accepted) {
            callbackState_->timerSetupFailures.fetch_add(
                1, std::memory_order_relaxed);
            cell->cadenceSetupPending.store(false, std::memory_order_release);
            cell->timerRetired.store(true, std::memory_order_release);
            revokeProfile(callbackState_, true);
            if (posted == gamenet::net::PostResult::QueueFull) {
                throw std::overflow_error(
                    "sharded Hybrid cadence setup queue is full");
            }
            throw std::logic_error(
                "sharded Hybrid cadence owner is unavailable");
        }
    }

    try {
        server_.start();
        started_ = true;
    } catch (...) {
        revokeProfile(callbackState_, true);
        throw;
    }
}

void MultiIoShardedHybrid::stop() {
    (void)stopGracefully(gamenet::net::TcpServerStopOptions{
        .drainTimeout = std::chrono::milliseconds::zero(),
    });
}

MultiIoShardedHybridStopHandle MultiIoShardedHybrid::stopGracefully(
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

const gamenet::net::InetAddress& MultiIoShardedHybrid::listenAddress()
    const noexcept {
    return server_.listenAddress();
}

std::size_t MultiIoShardedHybrid::logicShardForKey(
    const LogicShardKey& key) const noexcept {
    return callbackState_->selectLogicShard(key);
}

MultiIoShardedHybridMetrics MultiIoShardedHybrid::metrics() const {
    std::size_t networkOwnerCount = 0;
    {
        std::lock_guard lock(callbackState_->routesMutex);
        networkOwnerCount = callbackState_->networkOwners.size();
    }

    std::vector<ShardedHybridCellMetrics> cells;
    cells.reserve(callbackState_->cells.size());
    for (const auto& cell : callbackState_->cells) {
        const auto snapshot = cell->queue.snapshot();
        cells.push_back({
            .index = cell->index,
            .logicOwnerExecutorId = cell->executor.id(),
            .depth = snapshot.depth,
            .queuedBytes = snapshot.queuedBytes,
            .depthHighWatermark = snapshot.depthHighWatermark,
            .bytesHighWatermark = snapshot.bytesHighWatermark,
            .accepted = snapshot.accepted,
            .eventAccepted = snapshot.eventAccepted,
            .fixedAccepted = snapshot.fixedAccepted,
            .droppedOnStop = snapshot.droppedOnStop +
                cell->committedDrops.load(),
            .droppedBytesOnStop = snapshot.droppedBytesOnStop +
                cell->committedDropBytes.load(),
            .eventDrainCallbacks = cell->eventDrainCallbacks.load(),
            .eventDrainContinuations = cell->eventDrainContinuations.load(),
            .fixedTicks = cell->fixedTicks.load(),
            .emptyTicks = cell->emptyTicks.load(),
            .fixedHeadDeferrals = cell->fixedHeadDeferrals.load(),
            .handlerCalls = cell->handlerCalls.load(),
            .maximumSequence = snapshot.maximumSequence,
            .queueAgeP99Us = cell->queueAge.percentile(990),
            .queueAgeP999Us = cell->queueAge.percentile(999),
            .queueAgeMaxUs = cell->queueAgeMaxUs.load(),
            .accepting = snapshot.accepting,
            .timerRetired = cell->timerRetired.load(),
        });
    }

    return {
        .connectionsOpened = callbackState_->connectionsOpened.load(),
        .connectionsClosed = callbackState_->connectionsClosed.load(),
        .networkOwnerCount = networkOwnerCount,
        .logicCellCount = callbackState_->cells.size(),
        .stableHashSelections = callbackState_->stableHashSelections.load(),
        .commandsAccepted = callbackState_->commandsAccepted.load(),
        .eventCommandsAccepted =
            callbackState_->eventCommandsAccepted.load(),
        .fixedCommandsAccepted =
            callbackState_->fixedCommandsAccepted.load(),
        .queueFullRejections = callbackState_->queueFullRejections.load(),
        .payloadTooLargeRejections =
            callbackState_->payloadTooLargeRejections.load(),
        .shardKeyRejections = callbackState_->shardKeyRejections.load(),
        .stoppedRejections = callbackState_->stoppedRejections.load(),
        .routerCalls = callbackState_->routerCalls.load(),
        .routerExceptions = callbackState_->routerExceptions.load(),
        .routerOverruns = callbackState_->routerOverruns.load(),
        .eventDrainPosts = callbackState_->eventDrainPosts.load(),
        .eventDrainMerges = callbackState_->eventDrainMerges.load(),
        .eventDrainPostRejections =
            callbackState_->eventDrainPostRejections.load(),
        .eventDrainContinuations =
            callbackState_->eventDrainContinuations.load(),
        .fixedHeadDeferrals = callbackState_->fixedHeadDeferrals.load(),
        .fixedTicks = callbackState_->fixedTicks.load(),
        .handlerCalls = callbackState_->handlerCalls.load(),
        .eventHandlerCalls = callbackState_->eventHandlerCalls.load(),
        .fixedHandlerCalls = callbackState_->fixedHandlerCalls.load(),
        .handlerExceptions = callbackState_->handlerExceptions.load(),
        .handlerOverruns = callbackState_->handlerOverruns.load(),
        .turnOverruns = callbackState_->turnOverruns.load(),
        .staleInputs = callbackState_->staleInputs.load(),
        .staleOutputs = callbackState_->staleOutputs.load(),
        .outputPosts = callbackState_->outputPosts.load(),
        .outputPostRejections =
            callbackState_->outputPostRejections.load(),
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
        .connectionOwnerMigrations =
            callbackState_->connectionOwnerMigrations.load(),
        .orderViolations = callbackState_->orderViolations.load(),
        .crossDomainHandoffs = callbackState_->crossDomainHandoffs.load(),
        .profileTerminalFailures =
            callbackState_->profileTerminalFailures.load(),
        .timerSetupFailures = callbackState_->timerSetupFailures.load(),
        .timerCancelFailures = callbackState_->timerCancelFailures.load(),
        .logicStopDroppedCommands =
            callbackState_->logicStopDroppedCommands.load(),
        .logicStopDroppedBytes =
            callbackState_->logicStopDroppedBytes.load(),
        .queueAgeP99Us = callbackState_->queueAge.percentile(990),
        .queueAgeP999Us = callbackState_->queueAge.percentile(999),
        .queueAgeMaxUs = callbackState_->queueAgeMaxUs.load(),
        .shutdownDrainWaitUs = callbackState_->shutdownDrainWaitUs.load(),
        .cells = std::move(cells),
    };
}

void MultiIoShardedHybrid::handleFramerResult(
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
        result.status ==
            gamenet::protocol::FrameStatus::BufferLimitExceeded ||
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

void MultiIoShardedHybrid::submitFrame(
    const std::shared_ptr<ConnectionState>& state,
    std::string payload) {
    const auto callbackState = state->callbackState;
    const auto generation =
        state->route->generation.load(std::memory_order_acquire);

    MultiIoShardedHybridRoute routed;
    const auto routerStartedAt = Clock::now();
    callbackState->routerCalls.fetch_add(1, std::memory_order_relaxed);
    try {
        routed = callbackState->router(
            state->route->endpoint->id(), payload);
    } catch (...) {
        callbackState->routerExceptions.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::ProtocolError);
        return;
    }
    if (Clock::now() - routerStartedAt >
        callbackState->options.maxRouterWallTime) {
        callbackState->routerOverruns.fetch_add(1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::Overloaded);
        return;
    }
    if (!validLane(routed.lane) || !validKeyKind(routed.key.kind) ||
        routed.key.value.empty() ||
        routed.key.value.size() > callbackState->options.maxShardKeyBytes) {
        callbackState->shardKeyRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::ProtocolError);
        return;
    }
    if (!callbackState->active.load(std::memory_order_acquire) ||
        !state->route->isCurrent(generation)) {
        callbackState->stoppedRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::GoingAway);
        return;
    }

    const auto shard = callbackState->selectLogicShard(routed.key);
    callbackState->stableHashSelections.fetch_add(
        1, std::memory_order_relaxed);
    const auto cell = callbackState->cells[shard];
    ShardedHybridCommand command{
        .transportId = state->route->endpoint->id(),
        .generation = generation,
        .connectionOwnerExecutorId = state->route->ownerExecutor.id(),
        .key = std::move(routed.key),
        .lane = routed.lane,
        .payload = std::move(payload),
        .enqueuedAt = Clock::now(),
    };
    const auto outcome = cell->queue.submit(std::move(command));
    switch (outcome.result) {
    case CellQueue::SubmitResult::Accepted:
        callbackState->commandsAccepted.fetch_add(
            1, std::memory_order_relaxed);
        callbackState->crossDomainHandoffs.fetch_add(
            1, std::memory_order_relaxed);
        if (routed.lane == HybridDispatchLane::EventDriven) {
            callbackState->eventCommandsAccepted.fetch_add(
                1, std::memory_order_relaxed);
            scheduleEventDrain(callbackState, cell, false);
        } else {
            callbackState->fixedCommandsAccepted.fetch_add(
                1, std::memory_order_relaxed);
        }
        return;
    case CellQueue::SubmitResult::QueueFull:
        callbackState->queueFullRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::Overloaded);
        return;
    case CellQueue::SubmitResult::PayloadTooLarge:
        callbackState->payloadTooLargeRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::ProtocolError);
        return;
    case CellQueue::SubmitResult::KeyInvalid:
        callbackState->shardKeyRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::ProtocolError);
        return;
    case CellQueue::SubmitResult::Stopped:
        callbackState->stoppedRejections.fetch_add(
            1, std::memory_order_relaxed);
        state->closing = true;
        state->route->revoke();
        (void)state->route->endpoint->close(
            gamenet::transport::CloseReason::GoingAway);
        return;
    }
}

void MultiIoShardedHybrid::scheduleEventDrain(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell,
    bool continuation) {
    if (cell->eventDrainScheduled.exchange(true, std::memory_order_acq_rel)) {
        if (!continuation) {
            state->eventDrainMerges.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const auto posted = cell->executor.post(
        [state, cell] { runEventDrain(state, cell); });
    if (posted == gamenet::net::PostResult::Accepted) {
        if (continuation) {
            state->eventDrainContinuations.fetch_add(
                1, std::memory_order_relaxed);
            cell->eventDrainContinuations.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            state->eventDrainPosts.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    state->eventDrainPostRejections.fetch_add(
        1, std::memory_order_relaxed);
    cell->eventDrainScheduled.store(false, std::memory_order_release);
    revokeProfile(state, true);
}

void MultiIoShardedHybrid::runEventDrain(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell) {
    cell->callbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto finish = [&] {
        if (cell->callbacksInFlight.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            completeLogicStop(state);
        }
    };
    if (!cell->executor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    cell->eventDrainCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!state->active.load(std::memory_order_acquire)) {
        cell->eventDrainScheduled.store(false, std::memory_order_release);
        finish();
        return;
    }

    const auto turnStartedAt = Clock::now();
    auto drained = cell->queue.drainEventPrefix(
        state->options.maxCommandsPerEventDrain);
    if (drained.blockedByFixed) {
        state->fixedHeadDeferrals.fetch_add(1, std::memory_order_relaxed);
        cell->fixedHeadDeferrals.fetch_add(1, std::memory_order_relaxed);
    }
    processCommands(
        state,
        cell,
        std::move(drained.commands),
        false,
        0,
        turnStartedAt);

    cell->eventDrainScheduled.store(false, std::memory_order_release);
    if (state->active.load(std::memory_order_acquire) &&
        cell->queue.hasEventHead()) {
        scheduleEventDrain(state, cell, true);
    }
    finish();
}

void MultiIoShardedHybrid::installCellCadence(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell) {
    if (!cell->executor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (!state->active.load(std::memory_order_acquire)) {
        cell->cadenceSetupPending.store(false, std::memory_order_release);
        cell->timerRetired.store(true, std::memory_order_release);
        completeLogicStop(state);
        return;
    }
    const auto scheduled = cell->logicLoop->tryRunEvery(
        state->options.tickInterval,
        [state, cell] { runCellTick(state, cell); },
        gamenet::net::RepeatingTimerOptions{
            .mode = gamenet::net::RepeatingTimerMode::FixedRate,
            .maxCatchUpCallbacks = 0,
        });
    if (scheduled.result == gamenet::net::PostResult::Accepted) {
        cell->timer = scheduled.timerId;
        cell->cadenceSetupPending.store(false, std::memory_order_release);
        return;
    }
    state->timerSetupFailures.fetch_add(1, std::memory_order_relaxed);
    state->terminalCellFailure.store(true, std::memory_order_release);
    cell->cadenceSetupPending.store(false, std::memory_order_release);
    cell->timerRetired.store(true, std::memory_order_release);
    revokeProfile(state, true);
}

void MultiIoShardedHybrid::runCellTick(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell) {
    cell->callbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto finish = [&] {
        if (cell->callbacksInFlight.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            completeLogicStop(state);
        }
    };
    if (!cell->executor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (!state->active.load(std::memory_order_acquire)) {
        stopCellOnOwner(state, cell);
        finish();
        return;
    }

    const auto turnStartedAt = Clock::now();
    const auto tickSequence =
        cell->fixedTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    state->fixedTicks.fetch_add(1, std::memory_order_relaxed);
    auto commands = cell->queue.drainTickPrefix(
        state->options.maxCommandsPerTick);
    if (commands.empty()) {
        cell->emptyTicks.fetch_add(1, std::memory_order_relaxed);
    }
    processCommands(
        state,
        cell,
        std::move(commands),
        true,
        tickSequence,
        turnStartedAt);
    if (state->active.load(std::memory_order_acquire) &&
        cell->queue.hasEventHead()) {
        scheduleEventDrain(state, cell, true);
    }
    finish();
}

void MultiIoShardedHybrid::processCommands(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell,
    std::vector<ShardedHybridCommand> commands,
    bool dispatchedByTick,
    std::uint64_t tickSequence,
    Clock::time_point turnStartedAt) {
    for (std::size_t index = 0; index < commands.size(); ++index) {
        auto& command = commands[index];
        if (!state->active.load(std::memory_order_acquire)) {
            std::size_t bytes = 0;
            for (std::size_t remaining = index;
                 remaining < commands.size();
                 ++remaining) {
                bytes += commands[remaining].queuedBytes();
            }
            state->accountDrops(cell, commands.size() - index, bytes);
            break;
        }

        if (command.sequence <= cell->ownerLastHandledSequence) {
            state->orderViolations.fetch_add(1, std::memory_order_relaxed);
        }
        cell->ownerLastHandledSequence = command.sequence;

        const auto startedAt = Clock::now();
        const auto age = startedAt - command.enqueuedAt;
        state->queueAge.record(age);
        cell->queueAge.record(age);
        updateMaximum(state->queueAgeMaxUs, elapsedMicros(age));
        updateMaximum(cell->queueAgeMaxUs, elapsedMicros(age));
        auto route = state->findCurrentRoute(
            command.transportId, command.generation);
        if (!route) {
            state->staleInputs.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (route->ownerExecutor.id() != command.connectionOwnerExecutorId) {
            state->connectionOwnerMigrations.fetch_add(
                1, std::memory_order_relaxed);
            route->revoke();
            (void)route->endpoint->requestClose(
                gamenet::transport::CloseReason::GoingAway);
            continue;
        }

        const MultiIoShardedHybridContext context{
            .logicShardIndex = cell->index,
            .cellSequence = command.sequence,
            .tickSequence = tickSequence,
            .connectionOwnerExecutorId = command.connectionOwnerExecutorId,
            .logicOwnerExecutorId = cell->executor.id(),
            .lane = command.lane,
            .dispatchedByTick = dispatchedByTick,
            .shardKey = command.key,
            .enqueuedAt = command.enqueuedAt,
            .startedAt = startedAt,
        };
        MultiIoShardedHybridHandlerResult handlerResult;
        state->handlerCalls.fetch_add(1, std::memory_order_relaxed);
        cell->handlerCalls.fetch_add(1, std::memory_order_relaxed);
        if (command.lane == HybridDispatchLane::EventDriven) {
            state->eventHandlerCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            state->fixedHandlerCalls.fetch_add(1, std::memory_order_relaxed);
        }
        const auto handlerStartedAt = Clock::now();
        try {
            handlerResult = state->handler(
                context, command.transportId, command.payload);
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
            handlerResult.action == MultiIoShardedHybridAction::Close;
        if (!hasOwnerOutput) continue;
        if (!state->active.load(std::memory_order_acquire) ||
            !route->isCurrent(command.generation)) {
            state->staleOutputs.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::optional<std::string> encoded;
        if (handlerResult.reply) {
            encoded = state->encoder.encode(*handlerResult.reply);
            if (!encoded) {
                state->protocolFailures.fetch_add(
                    1, std::memory_order_relaxed);
                route->revoke();
                (void)route->endpoint->requestClose(
                    gamenet::transport::CloseReason::ProtocolError);
                continue;
            }
        }
        const auto generation = command.generation;
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
                    state->staleOutputs.fetch_add(
                        1, std::memory_order_relaxed);
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
                if (action == MultiIoShardedHybridAction::Close) {
                    route->revoke();
                    (void)route->endpoint->close(
                        gamenet::transport::CloseReason::Normal);
                }
            });
        if (posted == gamenet::net::PostResult::Accepted) {
            state->outputPosts.fetch_add(1, std::memory_order_relaxed);
            state->crossDomainHandoffs.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            state->outputPostRejections.fetch_add(
                1, std::memory_order_relaxed);
            route->revoke();
            (void)route->endpoint->requestClose(
                posted == gamenet::net::PostResult::QueueFull
                    ? gamenet::transport::CloseReason::Overloaded
                    : gamenet::transport::CloseReason::GoingAway);
        }
    }

    if (Clock::now() - turnStartedAt > state->options.maxTurnWallTime) {
        state->turnOverruns.fetch_add(1, std::memory_order_relaxed);
    }
}

void MultiIoShardedHybrid::stopCellOnOwner(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell) {
    if (!cell->executor.isInOwnerThread()) {
        state->logicOwnerViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (cell->timer.valid()) {
        const auto result = cell->logicLoop->tryCancel(cell->timer);
        if (result != gamenet::net::PostResult::Accepted) {
            state->timerCancelFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
        cell->timer = {};
    }
    cell->cadenceSetupPending.store(false, std::memory_order_release);
    cell->timerRetired.store(true, std::memory_order_release);
    completeLogicStop(state);
}

void MultiIoShardedHybrid::requestCellStop(
    const std::shared_ptr<CallbackState>& state,
    const std::shared_ptr<CellState>& cell) {
    if (cell->timerRetired.load(std::memory_order_acquire) &&
        !cell->cadenceSetupPending.load(std::memory_order_acquire)) {
        completeLogicStop(state);
        return;
    }
    if (cell->stopRequested.exchange(true, std::memory_order_acq_rel)) {
        completeLogicStop(state);
        return;
    }
    const auto posted = cell->executor.post(
        [state, cell] { stopCellOnOwner(state, cell); });
    if (posted == gamenet::net::PostResult::Accepted ||
        posted == gamenet::net::PostResult::QueueFull) {
        if (posted == gamenet::net::PostResult::QueueFull) {
            state->timerCancelFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
        return;
    }
    state->timerCancelFailures.fetch_add(1, std::memory_order_relaxed);
    cell->cadenceSetupPending.store(false, std::memory_order_release);
    cell->timerRetired.store(true, std::memory_order_release);
    cell->eventDrainScheduled.store(false, std::memory_order_release);
    completeLogicStop(state);
}

void MultiIoShardedHybrid::revokeProfile(
    const std::shared_ptr<CallbackState>& state,
    bool terminalCellFailure) {
    if (terminalCellFailure) {
        state->terminalCellFailure.store(true, std::memory_order_release);
    }
    const bool wasActive =
        state->active.exchange(false, std::memory_order_acq_rel);
    state->markShutdownStarted();
    if (terminalCellFailure && wasActive) {
        state->profileTerminalFailures.fetch_add(
            1, std::memory_order_relaxed);
    }
    for (const auto& cell : state->cells) {
        if (cell->callbacksInFlight.load(std::memory_order_acquire) != 0) {
            state->activeCallbacksObserved.store(
                true, std::memory_order_release);
        }
    }

    if (wasActive) {
        for (const auto& cell : state->cells) {
            const auto discarded = cell->queue.closeAndDiscard();
            state->logicStopDroppedCommands.fetch_add(
                discarded.commands, std::memory_order_relaxed);
            state->logicStopDroppedBytes.fetch_add(
                discarded.bytes, std::memory_order_relaxed);
        }

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
                terminalCellFailure
                    ? gamenet::transport::CloseReason::Overloaded
                    : gamenet::transport::CloseReason::GoingAway);
        }
    }

    for (const auto& cell : state->cells) {
        requestCellStop(state, cell);
    }
    completeLogicStop(state);
}

void MultiIoShardedHybrid::completeLogicStop(
    const std::shared_ptr<CallbackState>& state) {
    if (state->active.load(std::memory_order_acquire)) return;
    std::size_t cellsRetired = 0;
    for (const auto& cell : state->cells) {
        if (cell->cadenceSetupPending.load(std::memory_order_acquire) ||
            !cell->timerRetired.load(std::memory_order_acquire) ||
            cell->eventDrainScheduled.load(std::memory_order_acquire) ||
            cell->callbacksInFlight.load(std::memory_order_acquire) != 0) {
            return;
        }
        ++cellsRetired;
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
        .cellsRetired = cellsRetired,
        .activeCallbacksObserved = state->activeCallbacksObserved.load(),
        .terminalCellFailure = state->terminalCellFailure.load(),
        .shutdownDrainWaitUs = elapsed,
    });
}

}  // namespace gamenet::examples
