#pragma once

#include "gamenet/core/net/InetAddress.h"
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

enum class MultiIoQueuedAction {
    Continue,
    Close,
};

struct MultiIoQueuedHandlerResult {
    MultiIoQueuedAction action{MultiIoQueuedAction::Continue};
    std::optional<std::string> reply;
};

using MultiIoQueuedHandler = std::function<MultiIoQueuedHandlerResult(
    gamenet::transport::TransportSessionId,
    std::string_view)>;

struct MultiIoQueuedEventOptions {
    int ioThreads{2};
    gamenet::protocol::PacketFramerOptions framing{
        .maxPayloadBytes = 64U * 1024U,
        .maxBufferedBytes = 2U * (64U * 1024U + sizeof(std::uint32_t)),
        .maxFramesPerPush = 32,
        .maxFrameBytesPerPush = 256U * 1024U,
    };
    gamenet::game_logic::QueueLimits queueLimits{};
    std::size_t maxCommandsPerDrain{64};
    gamenet::net::TcpServerAdmissionOptions admission{};
    gamenet::net::TcpConnectionBackpressureOptions connectionBackpressure{};
    std::chrono::steady_clock::duration maxHandlerWallTime{
        std::chrono::milliseconds(5)};
};

struct MultiIoQueuedLogicStopSummary {
    std::size_t droppedCommands{};
    std::size_t droppedBytes{};
    bool terminalWakeFailure{};
};

struct MultiIoQueuedStopHandle {
    gamenet::net::TcpServerStopFuture networkStop;
    std::shared_future<MultiIoQueuedLogicStopSummary> logicStop;

    bool valid() const noexcept {
        return networkStop.valid() && logicStop.valid();
    }
};

struct MultiIoQueuedEventMetrics {
    std::uint64_t connectionsOpened{};
    std::uint64_t connectionsClosed{};
    std::size_t networkOwnerCount{};
    std::uint64_t commandsAccepted{};
    std::uint64_t queueFullRejections{};
    std::uint64_t payloadTooLargeRejections{};
    std::uint64_t stoppedRejections{};
    std::uint64_t producerWakePosts{};
    std::uint64_t producerWakeMerges{};
    std::uint64_t producerWakeRejections{};
    std::uint64_t drainCallbacks{};
    std::uint64_t drainContinuations{};
    std::uint64_t drainContinuationRejections{};
    std::size_t maxCommandsInDrain{};
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
    std::size_t logicStopDroppedCommands{};
    std::size_t logicStopDroppedBytes{};
    std::uint64_t networkToLogicP99Us{};
    std::uint64_t networkToLogicP999Us{};
    std::uint64_t logicToNetworkP99Us{};
    std::uint64_t logicToNetworkP999Us{};
    std::uint64_t queueOldestAgeMaxUs{};
    gamenet::game_logic::QueueSnapshot queue;
};

class MultiIoQueuedEvent {
public:
    MultiIoQueuedEvent(
        gamenet::net::EventLoop* baseLoop,
        gamenet::net::EventLoop* logicLoop,
        const gamenet::net::InetAddress& listenAddress,
        MultiIoQueuedHandler handler,
        MultiIoQueuedEventOptions options = {});
    ~MultiIoQueuedEvent();

    MultiIoQueuedEvent(const MultiIoQueuedEvent&) = delete;
    MultiIoQueuedEvent& operator=(const MultiIoQueuedEvent&) = delete;

    void start();
    void stop();
    MultiIoQueuedStopHandle stopGracefully(
        gamenet::net::TcpServerStopOptions options = {});

    const gamenet::net::InetAddress& listenAddress() const noexcept;
    MultiIoQueuedEventMetrics metrics() const;

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
    static void scheduleProducerDrain(
        const std::shared_ptr<CallbackState>& state);
    static void drainQueue(const std::shared_ptr<CallbackState>& state);
    static void revokeProfile(
        const std::shared_ptr<CallbackState>& state,
        bool terminalWakeFailure);
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
