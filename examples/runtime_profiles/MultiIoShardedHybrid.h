#pragma once

#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/transport/TransportEndpoint.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::examples {

enum class ConnectionPlacementPolicy {
    RoundRobin,
    LeastConnections,
    QueueLag,
    ConsistentHash,
};

enum class LogicShardPolicy {
    StableHash,
};

enum class LogicShardKeyKind : std::uint8_t {
    Player,
    Room,
    Scene,
};

struct LogicShardKey {
    LogicShardKeyKind kind{LogicShardKeyKind::Player};
    std::string value;
};

enum class HybridDispatchLane : std::uint8_t {
    EventDriven,
    FixedTick,
};

struct MultiIoShardedHybridRoute {
    LogicShardKey key;
    HybridDispatchLane lane{HybridDispatchLane::EventDriven};
};

struct MultiIoShardedHybridContext {
    std::size_t logicShardIndex{};
    std::uint64_t cellSequence{};
    std::uint64_t tickSequence{};
    std::uint64_t connectionOwnerExecutorId{};
    std::uint64_t logicOwnerExecutorId{};
    HybridDispatchLane lane{HybridDispatchLane::EventDriven};
    bool dispatchedByTick{};
    LogicShardKey shardKey;
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point startedAt{};
};

enum class MultiIoShardedHybridAction {
    Continue,
    Close,
};

struct MultiIoShardedHybridHandlerResult {
    MultiIoShardedHybridAction action{MultiIoShardedHybridAction::Continue};
    std::optional<std::string> reply;
};

using MultiIoShardedHybridRouter = std::function<MultiIoShardedHybridRoute(
    gamenet::transport::TransportSessionId,
    std::string_view)>;

using MultiIoShardedHybridHandler =
    std::function<MultiIoShardedHybridHandlerResult(
        const MultiIoShardedHybridContext&,
        gamenet::transport::TransportSessionId,
        std::string_view)>;

struct MultiIoShardedHybridOptions {
    int ioThreads{2};
    ConnectionPlacementPolicy connectionPlacement{
        ConnectionPlacementPolicy::RoundRobin};
    LogicShardPolicy logicShardPolicy{LogicShardPolicy::StableHash};
    gamenet::protocol::PacketFramerOptions framing{
        .maxPayloadBytes = 64U * 1024U,
        .maxBufferedBytes = 2U * (64U * 1024U + sizeof(std::uint32_t)),
        .maxFramesPerPush = 32,
        .maxFrameBytesPerPush = 256U * 1024U,
    };
    std::size_t maxCommandsPerCell{4096};
    std::size_t maxQueuedBytesPerCell{4U * 1024U * 1024U};
    std::size_t maxPayloadBytes{1024U * 1024U};
    std::size_t maxShardKeyBytes{256};
    std::size_t maxCommandsPerEventDrain{64};
    std::size_t maxCommandsPerTick{256};
    std::chrono::steady_clock::duration tickInterval{
        std::chrono::milliseconds(20)};
    gamenet::net::TcpServerAdmissionOptions admission{};
    gamenet::net::TcpConnectionBackpressureOptions connectionBackpressure{};
    std::chrono::steady_clock::duration maxRouterWallTime{
        std::chrono::milliseconds(1)};
    std::chrono::steady_clock::duration maxHandlerWallTime{
        std::chrono::milliseconds(5)};
    std::chrono::steady_clock::duration maxTurnWallTime{
        std::chrono::milliseconds(20)};
};

struct ShardedHybridCellMetrics {
    std::size_t index{};
    std::uint64_t logicOwnerExecutorId{};
    std::size_t depth{};
    std::size_t queuedBytes{};
    std::size_t depthHighWatermark{};
    std::size_t bytesHighWatermark{};
    std::uint64_t accepted{};
    std::uint64_t eventAccepted{};
    std::uint64_t fixedAccepted{};
    std::uint64_t droppedOnStop{};
    std::uint64_t droppedBytesOnStop{};
    std::uint64_t eventDrainCallbacks{};
    std::uint64_t eventDrainContinuations{};
    std::uint64_t fixedTicks{};
    std::uint64_t emptyTicks{};
    std::uint64_t fixedHeadDeferrals{};
    std::uint64_t handlerCalls{};
    std::uint64_t maximumSequence{};
    std::uint64_t queueAgeP99Us{};
    std::uint64_t queueAgeP999Us{};
    std::uint64_t queueAgeMaxUs{};
    bool accepting{};
    bool timerRetired{};
};

struct MultiIoShardedHybridLogicStopSummary {
    std::size_t droppedCommands{};
    std::size_t droppedBytes{};
    std::size_t cellsRetired{};
    bool activeCallbacksObserved{};
    bool terminalCellFailure{};
    std::uint64_t shutdownDrainWaitUs{};
};

struct MultiIoShardedHybridStopHandle {
    gamenet::net::TcpServerStopFuture networkStop;
    std::shared_future<MultiIoShardedHybridLogicStopSummary> logicStop;

    bool valid() const noexcept {
        return networkStop.valid() && logicStop.valid();
    }
};

struct MultiIoShardedHybridMetrics {
    std::uint64_t connectionsOpened{};
    std::uint64_t connectionsClosed{};
    std::size_t networkOwnerCount{};
    std::size_t logicCellCount{};
    std::uint64_t stableHashSelections{};
    std::uint64_t commandsAccepted{};
    std::uint64_t eventCommandsAccepted{};
    std::uint64_t fixedCommandsAccepted{};
    std::uint64_t queueFullRejections{};
    std::uint64_t payloadTooLargeRejections{};
    std::uint64_t shardKeyRejections{};
    std::uint64_t stoppedRejections{};
    std::uint64_t routerCalls{};
    std::uint64_t routerExceptions{};
    std::uint64_t routerOverruns{};
    std::uint64_t eventDrainPosts{};
    std::uint64_t eventDrainMerges{};
    std::uint64_t eventDrainPostRejections{};
    std::uint64_t eventDrainContinuations{};
    std::uint64_t fixedHeadDeferrals{};
    std::uint64_t fixedTicks{};
    std::uint64_t handlerCalls{};
    std::uint64_t eventHandlerCalls{};
    std::uint64_t fixedHandlerCalls{};
    std::uint64_t handlerExceptions{};
    std::uint64_t handlerOverruns{};
    std::uint64_t turnOverruns{};
    std::uint64_t staleInputs{};
    std::uint64_t staleOutputs{};
    std::uint64_t outputPosts{};
    std::uint64_t outputPostRejections{};
    std::uint64_t outputsAccepted{};
    std::uint64_t outputOverloads{};
    std::uint64_t protocolFailures{};
    std::uint64_t framerContinuationPosts{};
    std::uint64_t framerContinuationRejections{};
    std::uint64_t networkOwnerViolations{};
    std::uint64_t logicOwnerViolations{};
    std::uint64_t endpointOwnerViolations{};
    std::uint64_t connectionOwnerMigrations{};
    std::uint64_t orderViolations{};
    std::uint64_t crossDomainHandoffs{};
    std::uint64_t profileTerminalFailures{};
    std::uint64_t timerSetupFailures{};
    std::uint64_t timerCancelFailures{};
    std::size_t logicStopDroppedCommands{};
    std::size_t logicStopDroppedBytes{};
    std::uint64_t queueAgeP99Us{};
    std::uint64_t queueAgeP999Us{};
    std::uint64_t queueAgeMaxUs{};
    std::uint64_t shutdownDrainWaitUs{};
    std::vector<ShardedHybridCellMetrics> cells;
};

class MultiIoShardedHybrid {
public:
    MultiIoShardedHybrid(
        gamenet::net::EventLoop* baseLoop,
        std::vector<gamenet::net::EventLoop*> logicLoops,
        const gamenet::net::InetAddress& listenAddress,
        MultiIoShardedHybridRouter router,
        MultiIoShardedHybridHandler handler,
        MultiIoShardedHybridOptions options = {});
    ~MultiIoShardedHybrid();

    MultiIoShardedHybrid(const MultiIoShardedHybrid&) = delete;
    MultiIoShardedHybrid& operator=(const MultiIoShardedHybrid&) = delete;

    void start();
    void stop();
    MultiIoShardedHybridStopHandle stopGracefully(
        gamenet::net::TcpServerStopOptions options = {});

    const gamenet::net::InetAddress& listenAddress() const noexcept;
    std::size_t logicShardForKey(const LogicShardKey& key) const noexcept;
    MultiIoShardedHybridMetrics metrics() const;

private:
    struct CallbackState;
    class CellQueue;
    struct CellState;
    struct ConnectionRoute;
    struct ConnectionState;
    struct ShardedHybridCommand;

    static void handleFramerResult(
        const std::shared_ptr<ConnectionState>& state,
        gamenet::protocol::FrameResult result);
    static void submitFrame(
        const std::shared_ptr<ConnectionState>& state,
        std::string payload);
    static void scheduleEventDrain(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell,
        bool continuation);
    static void runEventDrain(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell);
    static void installCellCadence(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell);
    static void runCellTick(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell);
    static void processCommands(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell,
        std::vector<ShardedHybridCommand> commands,
        bool dispatchedByTick,
        std::uint64_t tickSequence,
        std::chrono::steady_clock::time_point turnStartedAt);
    static void stopCellOnOwner(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell);
    static void requestCellStop(
        const std::shared_ptr<CallbackState>& state,
        const std::shared_ptr<CellState>& cell);
    static void revokeProfile(
        const std::shared_ptr<CallbackState>& state,
        bool terminalCellFailure);
    static void completeLogicStop(
        const std::shared_ptr<CallbackState>& state);

    gamenet::net::EventLoop* baseLoop_;
    std::shared_ptr<CallbackState> callbackState_;
    gamenet::net::TcpServer server_;
    gamenet::net::TcpServerStopFuture networkStopFuture_;
    bool started_{false};
    bool stopped_{false};
};

}  // namespace gamenet::examples
