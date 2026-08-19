#pragma once

#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/game_logic/GameCommandQueue.h"
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

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::examples {

enum class FixedTickCadence {
    FixedRateSkipMissed,
    FixedRateBoundedCatchUp,
};

struct FixedTickContext {
    std::uint64_t tickSequence{};
    std::uint64_t cadenceIndex{};
    std::chrono::steady_clock::time_point scheduledAt{};
    std::chrono::steady_clock::time_point startedAt{};
    bool catchUp{};
};

enum class MultiIoDedicatedFixedTickAction {
    Continue,
    Close,
};

struct MultiIoDedicatedFixedTickHandlerResult {
    MultiIoDedicatedFixedTickAction action{
        MultiIoDedicatedFixedTickAction::Continue};
    std::optional<std::string> reply;
};

using MultiIoDedicatedFixedTickHandler =
    std::function<MultiIoDedicatedFixedTickHandlerResult(
        const FixedTickContext&,
        gamenet::transport::TransportSessionId,
        std::string_view)>;

struct MultiIoDedicatedFixedTickOptions {
    int ioThreads{2};
    gamenet::protocol::PacketFramerOptions framing{
        .maxPayloadBytes = 64U * 1024U,
        .maxBufferedBytes = 2U * (64U * 1024U + sizeof(std::uint32_t)),
        .maxFramesPerPush = 32,
        .maxFrameBytesPerPush = 256U * 1024U,
    };
    gamenet::game_logic::QueueLimits queueLimits{};
    std::chrono::steady_clock::duration tickInterval{
        std::chrono::milliseconds(20)};
    FixedTickCadence cadence{FixedTickCadence::FixedRateSkipMissed};
    std::size_t maxCatchUpTicks{0};
    std::size_t maxCommandsPerTick{256};
    gamenet::net::TcpServerAdmissionOptions admission{};
    gamenet::net::TcpConnectionBackpressureOptions connectionBackpressure{};
    std::chrono::steady_clock::duration maxHandlerWallTime{
        std::chrono::milliseconds(5)};
    std::chrono::steady_clock::duration maxTickWallTime{
        std::chrono::milliseconds(20)};
};

struct MultiIoDedicatedFixedTickLogicStopSummary {
    std::size_t droppedCommands{};
    std::size_t droppedBytes{};
    gamenet::net::PostResult cadenceStopPostResult{
        gamenet::net::PostResult::OwnerUnavailable};
    bool activeTickObserved{};
    bool terminalTimerFailure{};
    std::uint64_t shutdownDrainWaitUs{};
};

struct MultiIoDedicatedFixedTickStopHandle {
    gamenet::net::TcpServerStopFuture networkStop;
    std::shared_future<MultiIoDedicatedFixedTickLogicStopSummary> logicStop;

    bool valid() const noexcept {
        return networkStop.valid() && logicStop.valid();
    }
};

struct MultiIoDedicatedFixedTickMetrics {
    std::uint64_t connectionsOpened{};
    std::uint64_t connectionsClosed{};
    std::size_t networkOwnerCount{};
    std::uint64_t commandsAccepted{};
    std::uint64_t queueFullRejections{};
    std::uint64_t payloadTooLargeRejections{};
    std::uint64_t stoppedRejections{};
    std::uint64_t tickCount{};
    std::uint64_t cadenceIndex{};
    std::uint64_t emptyTicks{};
    std::uint64_t ticksWithWork{};
    std::uint64_t catchUpTicks{};
    std::uint64_t skippedTicks{};
    std::size_t maxConsecutiveCatchUp{};
    std::uint64_t tickOverruns{};
    std::size_t maxCommandsInTick{};
    std::uint64_t handlerCalls{};
    std::uint64_t handlerExceptions{};
    std::uint64_t handlerOverruns{};
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
    std::uint64_t crossDomainHandoffs{};
    std::uint64_t profileTerminalFailures{};
    std::uint64_t timerSetupPosts{};
    std::uint64_t timerSetupAccepted{};
    std::uint64_t timerSetupFailures{};
    std::uint64_t timerCancelPosts{};
    std::uint64_t timerCancelAccepted{};
    std::uint64_t timerCancelQueueFull{};
    std::uint64_t timerCancelUnavailable{};
    std::size_t logicStopDroppedCommands{};
    std::size_t logicStopDroppedBytes{};
    std::uint64_t tickJitterP50Us{};
    std::uint64_t tickJitterP99Us{};
    std::uint64_t tickJitterP999Us{};
    std::uint64_t tickDurationP99Us{};
    std::uint64_t tickDurationP999Us{};
    std::uint64_t queueAgeP99Us{};
    std::uint64_t queueAgeP999Us{};
    std::uint64_t queueAgeMaxUs{};
    std::uint64_t shutdownDrainWaitUs{};
    gamenet::game_logic::QueueSnapshot queue;
};

class MultiIoDedicatedFixedTick {
public:
    MultiIoDedicatedFixedTick(
        gamenet::net::EventLoop* baseLoop,
        gamenet::net::EventLoop* logicLoop,
        const gamenet::net::InetAddress& listenAddress,
        MultiIoDedicatedFixedTickHandler handler,
        MultiIoDedicatedFixedTickOptions options = {});
    ~MultiIoDedicatedFixedTick();

    MultiIoDedicatedFixedTick(const MultiIoDedicatedFixedTick&) = delete;
    MultiIoDedicatedFixedTick& operator=(
        const MultiIoDedicatedFixedTick&) = delete;

    void start();
    void stop();
    MultiIoDedicatedFixedTickStopHandle stopGracefully(
        gamenet::net::TcpServerStopOptions options = {});

    const gamenet::net::InetAddress& listenAddress() const noexcept;
    MultiIoDedicatedFixedTickMetrics metrics() const;

private:
    struct CallbackState;
    struct ConnectionRoute;
    struct ConnectionState;

    static void handleFramerResult(
        const std::shared_ptr<ConnectionState>& state,
        gamenet::protocol::FrameResult result);
    static void submitFrame(
        const std::shared_ptr<ConnectionState>& state,
        std::string payload);
    static void installCadenceOnOwner(
        const std::shared_ptr<CallbackState>& state);
    static void runTick(const std::shared_ptr<CallbackState>& state);
    static void stopCadenceOnOwner(
        const std::shared_ptr<CallbackState>& state);
    static void requestCadenceStop(
        const std::shared_ptr<CallbackState>& state);
    static void revokeProfile(
        const std::shared_ptr<CallbackState>& state,
        bool terminalTimerFailure);
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
