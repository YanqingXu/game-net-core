#include "runtime_profiles/MultiIoDedicatedFixedTick.h"
#include "runtime_profiles/MultiIoQueuedEvent.h"
#include "runtime_profiles/MultiIoShardedHybrid.h"
#include "runtime_profiles/SingleLoopInlineEvent.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::string_view kPayload = "common-profile-lifecycle";

struct NormalizedLifecycle {
    bool echoSucceeded{};
    bool exactlyOneConnectionRetired{};
    bool exactlyOneHandlerAndReply{};
    bool networkShutdownDrained{};
    bool logicShutdownDrained{};
    bool ownerAndGenerationSafe{};
    bool expectedHandoffModel{};

    bool operator==(const NormalizedLifecycle&) const = default;
};

constexpr NormalizedLifecycle kExpectedLifecycle{
    .echoSucceeded = true,
    .exactlyOneConnectionRetired = true,
    .exactlyOneHandlerAndReply = true,
    .networkShutdownDrained = true,
    .logicShutdownDrained = true,
    .ownerAndGenerationSafe = true,
    .expectedHandoffModel = true,
};

void assertExpected(
    std::string_view profile,
    const NormalizedLifecycle& observation) {
    if (observation != kExpectedLifecycle) {
        std::cerr << profile
                  << " normalized lifecycle: echo=" << observation.echoSucceeded
                  << ", connection=" << observation.exactlyOneConnectionRetired
                  << ", handler=" << observation.exactlyOneHandlerAndReply
                  << ", network=" << observation.networkShutdownDrained
                  << ", logic=" << observation.logicShutdownDrained
                  << ", owner=" << observation.ownerAndGenerationSafe
                  << ", handoff=" << observation.expectedHandoffModel << '\n';
    }
    GAMENET_TEST_ASSERT(observation == kExpectedLifecycle);
}

gamenet::protocol::PacketFramerOptions framingOptions() {
    return {
        .maxPayloadBytes = 512,
        .maxBufferedBytes = 4096,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 4096,
        .maxRetainedCapacityBytes = 516,
        .trimThresholdBytes = 256,
    };
}

gamenet::net::TcpConnectionBackpressureOptions backpressureOptions() {
    return {
        .lowWaterMarkBytes = 1024,
        .highWaterMarkBytes = 2048,
        .hardLimitBytes = 4096,
        .maxInputBufferBytes = 4096,
    };
}

std::string encodeFrame(
    const gamenet::protocol::PacketFramerOptions& options,
    std::string_view payload) {
    gamenet::protocol::PacketFramer encoder(options);
    auto encoded = encoder.encode(payload);
    GAMENET_TEST_ASSERT(encoded.has_value());
    return std::move(*encoded);
}

bool isConnectPendingWrite(int error) noexcept {
#ifdef _WIN32
    return error == WSAENOTCONN || error == WSAEALREADY;
#else
    return error == ENOTCONN || error == EALREADY;
#endif
}

bool writeEventually(gamenet::net::SocketFd fd, std::string_view bytes) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (offset < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInProgress(error) &&
            !gamenet::net::sockets::isInterrupted(error) &&
            !isConnectPendingWrite(error)) {
            std::cerr << "client write failed: "
                      << gamenet::net::sockets::errorMessage(error) << '\n';
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return offset == bytes.size();
}

bool readEchoEventually(
    gamenet::net::SocketFd fd,
    const gamenet::protocol::PacketFramerOptions& options) {
    gamenet::protocol::PacketFramer decoder(options);
    std::array<char, 2048> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(
            fd, bytes.data(), bytes.size());
        if (count > 0) {
            auto result = decoder.push(
                std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (true) {
                if (!result.frames.empty()) {
                    return result.frames.size() == 1 &&
                        result.frames.front() == kPayload;
                }
                if (!result.needsContinuation) break;
                result = decoder.push({});
            }
            continue;
        }
        if (count == 0) {
            std::cerr << "client peer closed before echo\n";
            return false;
        }
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            std::cerr << "client read failed: "
                      << gamenet::net::sockets::errorMessage(error) << '\n';
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool runEchoClient(
    const gamenet::net::InetAddress& address,
    const gamenet::protocol::PacketFramerOptions& framing) {
    const auto fd = gamenet::test::connectTestClient(address);
    const auto encoded = encodeFrame(framing, kPayload);
    const bool succeeded =
        writeEventually(fd, encoded) && readEchoEventually(fd, framing);
    gamenet::test::closeTestSocket(fd);
    return succeeded;
}

template <typename RequestStop, typename StopReady>
void driveClientAndStop(
    gamenet::net::EventLoop& loop,
    std::atomic<bool>& clientDone,
    RequestStop requestStop,
    StopReady stopReady,
    const char* timeoutMessage) {
    bool stopRequested = false;
    static_cast<void>(loop.runEvery(1ms, [&] {
        if (!clientDone.load(std::memory_order_acquire)) return;
        if (!stopRequested) {
            requestStop();
            stopRequested = true;
            return;
        }
        if (stopReady()) loop.quit();
    }));
    gamenet::test::runLoopWithTimeout(loop, 10s, timeoutMessage);
}

bool networkDrained(const gamenet::net::TcpServerStopFuture& future) {
    const auto result = future.get();
    return result.outcome == gamenet::net::TcpServerStopOutcome::Drained &&
        result.forcedConnectionCount == 0;
}

NormalizedLifecycle runProfileA() {
    gamenet::net::EventLoop loop;
    gamenet::examples::SingleLoopInlineEventOptions options{
        .framing = framingOptions(),
        .admission = {.maxConnections = 8},
        .connectionBackpressure = backpressureOptions(),
        .maxHandlerWallTime = 100ms,
    };
    gamenet::examples::SingleLoopInlineEvent profile(
        &loop,
        gamenet::net::InetAddress(0, true),
        [](auto, std::string_view payload) {
            return gamenet::examples::SingleLoopInlineHandlerResult{
                .action = gamenet::examples::SingleLoopInlineAction::Continue,
                .reply = std::string(payload),
            };
        },
        options);
    profile.start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        clientSucceeded.store(runEchoClient(profile.listenAddress(), options.framing));
        clientDone.store(true, std::memory_order_release);
    });
    gamenet::net::TcpServerStopFuture stop;
    driveClientAndStop(
        loop,
        clientDone,
        [&] { stop = profile.stopGracefully(); },
        [&] {
            return stop.valid() &&
                stop.wait_for(0s) == std::future_status::ready;
        },
        "Profile A common lifecycle timed out");
    client.join();

    const auto metrics = profile.metrics();
    return {
        .echoSucceeded = clientSucceeded.load(),
        .exactlyOneConnectionRetired =
            metrics.connectionsOpened == 1 && metrics.connectionsClosed == 1,
        .exactlyOneHandlerAndReply =
            metrics.handlerCalls == 1 && metrics.repliesAccepted == 1,
        .networkShutdownDrained = networkDrained(stop),
        .logicShutdownDrained = true,
        .ownerAndGenerationSafe =
            metrics.handlerExceptions == 0 && metrics.protocolFailures == 0,
        .expectedHandoffModel = metrics.crossDomainHandoffs == 0,
    };
}

NormalizedLifecycle runProfileB() {
    gamenet::net::EventLoopThread logicThread({}, "profile-common-b");
    auto* logicLoop = logicThread.startLoop();
    NormalizedLifecycle observation;
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoQueuedEventOptions options{
            .ioThreads = 2,
            .framing = framingOptions(),
            .queueLimits = {
                .maxCommands = 32,
                .maxQueuedBytes = 32U * 512U,
                .maxPayloadBytes = 512,
            },
            .maxCommandsPerDrain = 8,
            .admission = {.maxConnections = 8},
            .connectionBackpressure = backpressureOptions(),
            .maxHandlerWallTime = 100ms,
        };
        gamenet::examples::MultiIoQueuedEvent profile(
            &baseLoop,
            logicLoop,
            gamenet::net::InetAddress(0, true),
            [](auto, std::string_view payload) {
                return gamenet::examples::MultiIoQueuedHandlerResult{
                    .action = gamenet::examples::MultiIoQueuedAction::Continue,
                    .reply = std::string(payload),
                };
            },
            options);
        profile.start();

        std::atomic<bool> clientDone{false};
        std::atomic<bool> clientSucceeded{false};
        std::thread client([&] {
            clientSucceeded.store(runEchoClient(profile.listenAddress(), options.framing));
            clientDone.store(true, std::memory_order_release);
        });
        gamenet::examples::MultiIoQueuedStopHandle stop;
        driveClientAndStop(
            baseLoop,
            clientDone,
            [&] { stop = profile.stopGracefully(); },
            [&] {
                return stop.valid() &&
                    stop.networkStop.wait_for(0s) == std::future_status::ready &&
                    stop.logicStop.wait_for(0s) == std::future_status::ready;
            },
            "Profile B common lifecycle timed out");
        client.join();

        const auto summary = stop.logicStop.get();
        const auto metrics = profile.metrics();
        observation = {
            .echoSucceeded = clientSucceeded.load(),
            .exactlyOneConnectionRetired =
                metrics.connectionsOpened == 1 && metrics.connectionsClosed == 1,
            .exactlyOneHandlerAndReply =
                metrics.handlerCalls == 1 && metrics.outputsAccepted == 1,
            .networkShutdownDrained = networkDrained(stop.networkStop),
            .logicShutdownDrained =
                summary.droppedCommands == 0 && summary.droppedBytes == 0 &&
                !summary.terminalWakeFailure,
            .ownerAndGenerationSafe =
                metrics.networkOwnerViolations == 0 &&
                metrics.logicOwnerViolations == 0 &&
                metrics.endpointOwnerViolations == 0 &&
                metrics.staleInputs == 0 && metrics.staleOutputs == 0 &&
                metrics.profileTerminalFailures == 0,
            .expectedHandoffModel = metrics.crossDomainHandoffs == 2,
        };
    }
    logicThread.stop();
    return observation;
}

NormalizedLifecycle runProfileC() {
    gamenet::net::EventLoopThread logicThread({}, "profile-common-c");
    auto* logicLoop = logicThread.startLoop();
    NormalizedLifecycle observation;
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoDedicatedFixedTickOptions options{
            .ioThreads = 2,
            .framing = framingOptions(),
            .queueLimits = {
                .maxCommands = 32,
                .maxQueuedBytes = 32U * 512U,
                .maxPayloadBytes = 512,
            },
            .tickInterval = 2ms,
            .cadence = gamenet::examples::FixedTickCadence::FixedRateSkipMissed,
            .maxCatchUpTicks = 0,
            .maxCommandsPerTick = 8,
            .admission = {.maxConnections = 8},
            .connectionBackpressure = backpressureOptions(),
            .maxHandlerWallTime = 100ms,
            .maxTickWallTime = 100ms,
        };
        gamenet::examples::MultiIoDedicatedFixedTick profile(
            &baseLoop,
            logicLoop,
            gamenet::net::InetAddress(0, true),
            [](const auto&, auto, std::string_view payload) {
                return gamenet::examples::MultiIoDedicatedFixedTickHandlerResult{
                    .action =
                        gamenet::examples::MultiIoDedicatedFixedTickAction::Continue,
                    .reply = std::string(payload),
                };
            },
            options);
        profile.start();

        std::atomic<bool> clientDone{false};
        std::atomic<bool> clientSucceeded{false};
        std::thread client([&] {
            clientSucceeded.store(runEchoClient(profile.listenAddress(), options.framing));
            clientDone.store(true, std::memory_order_release);
        });
        gamenet::examples::MultiIoDedicatedFixedTickStopHandle stop;
        driveClientAndStop(
            baseLoop,
            clientDone,
            [&] { stop = profile.stopGracefully(); },
            [&] {
                return stop.valid() &&
                    stop.networkStop.wait_for(0s) == std::future_status::ready &&
                    stop.logicStop.wait_for(0s) == std::future_status::ready;
            },
            "Profile C common lifecycle timed out");
        client.join();

        const auto summary = stop.logicStop.get();
        const auto metrics = profile.metrics();
        const bool cancellationAdmissionConverged =
            summary.cadenceStopPostResult == gamenet::net::PostResult::Accepted ||
            summary.cadenceStopPostResult == gamenet::net::PostResult::QueueFull;
        const bool logicShutdownDrained =
            summary.droppedCommands == 0 && summary.droppedBytes == 0 &&
            !summary.terminalTimerFailure && cancellationAdmissionConverged &&
            metrics.timerCancelAccepted >= 1 &&
            metrics.timerCancelUnavailable == 0;
        if (!logicShutdownDrained) {
            std::cerr << "Profile C logic stop: dropped_commands="
                      << summary.droppedCommands
                      << ", dropped_bytes=" << summary.droppedBytes
                      << ", terminal_timer_failure="
                      << summary.terminalTimerFailure
                      << ", cadence_stop_post_result="
                      << static_cast<int>(summary.cadenceStopPostResult)
                      << ", timer_cancel_posts=" << metrics.timerCancelPosts
                      << ", timer_cancel_accepted="
                      << metrics.timerCancelAccepted
                      << ", timer_cancel_queue_full="
                      << metrics.timerCancelQueueFull
                      << ", timer_cancel_unavailable="
                      << metrics.timerCancelUnavailable << '\n';
        }
        observation = {
            .echoSucceeded = clientSucceeded.load(),
            .exactlyOneConnectionRetired =
                metrics.connectionsOpened == 1 && metrics.connectionsClosed == 1,
            .exactlyOneHandlerAndReply =
                metrics.handlerCalls == 1 && metrics.outputsAccepted == 1,
            .networkShutdownDrained = networkDrained(stop.networkStop),
            .logicShutdownDrained = logicShutdownDrained,
            .ownerAndGenerationSafe =
                metrics.networkOwnerViolations == 0 &&
                metrics.logicOwnerViolations == 0 &&
                metrics.endpointOwnerViolations == 0 &&
                metrics.staleInputs == 0 && metrics.staleOutputs == 0 &&
                metrics.profileTerminalFailures == 0 && metrics.tickCount > 0 &&
                metrics.timerSetupAccepted == 1 && metrics.timerSetupFailures == 0,
            .expectedHandoffModel = metrics.crossDomainHandoffs == 2,
        };
    }
    logicThread.stop();
    return observation;
}

NormalizedLifecycle runProfileD() {
    gamenet::net::EventLoopThread firstLogicThread({}, "profile-common-d0");
    gamenet::net::EventLoopThread secondLogicThread({}, "profile-common-d1");
    auto* firstLogic = firstLogicThread.startLoop();
    auto* secondLogic = secondLogicThread.startLoop();
    NormalizedLifecycle observation;
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoShardedHybridOptions options{
            .ioThreads = 2,
            .connectionPlacement =
                gamenet::examples::ConnectionPlacementPolicy::RoundRobin,
            .logicShardPolicy = gamenet::examples::LogicShardPolicy::StableHash,
            .framing = framingOptions(),
            .maxCommandsPerCell = 32,
            .maxQueuedBytesPerCell = 32U * 512U,
            .maxPayloadBytes = 512,
            .maxShardKeyBytes = 64,
            .maxCommandsPerEventDrain = 8,
            .maxCommandsPerTick = 8,
            .tickInterval = 5ms,
            .admission = {.maxConnections = 8},
            .connectionBackpressure = backpressureOptions(),
            .maxRouterWallTime = 100ms,
            .maxHandlerWallTime = 100ms,
            .maxTurnWallTime = 100ms,
        };
        gamenet::examples::MultiIoShardedHybrid profile(
            &baseLoop,
            {firstLogic, secondLogic},
            gamenet::net::InetAddress(0, true),
            [](auto, std::string_view) {
                return gamenet::examples::MultiIoShardedHybridRoute{
                    .key = {
                        .kind = gamenet::examples::LogicShardKeyKind::Room,
                        .value = "common-profile",
                    },
                    .lane = gamenet::examples::HybridDispatchLane::EventDriven,
                };
            },
            [](const auto&, auto, std::string_view payload) {
                return gamenet::examples::MultiIoShardedHybridHandlerResult{
                    .action = gamenet::examples::MultiIoShardedHybridAction::Continue,
                    .reply = std::string(payload),
                };
            },
            options);
        profile.start();

        std::atomic<bool> clientDone{false};
        std::atomic<bool> clientSucceeded{false};
        std::thread client([&] {
            clientSucceeded.store(runEchoClient(profile.listenAddress(), options.framing));
            clientDone.store(true, std::memory_order_release);
        });
        gamenet::examples::MultiIoShardedHybridStopHandle stop;
        driveClientAndStop(
            baseLoop,
            clientDone,
            [&] { stop = profile.stopGracefully(); },
            [&] {
                return stop.valid() &&
                    stop.networkStop.wait_for(0s) == std::future_status::ready &&
                    stop.logicStop.wait_for(0s) == std::future_status::ready;
            },
            "Profile D common lifecycle timed out");
        client.join();

        const auto summary = stop.logicStop.get();
        const auto metrics = profile.metrics();
        observation = {
            .echoSucceeded = clientSucceeded.load(),
            .exactlyOneConnectionRetired =
                metrics.connectionsOpened == 1 && metrics.connectionsClosed == 1,
            .exactlyOneHandlerAndReply =
                metrics.handlerCalls == 1 && metrics.outputsAccepted == 1,
            .networkShutdownDrained = networkDrained(stop.networkStop),
            .logicShutdownDrained =
                summary.droppedCommands == 0 && summary.droppedBytes == 0 &&
                summary.cellsRetired == 2 && !summary.terminalCellFailure,
            .ownerAndGenerationSafe =
                metrics.networkOwnerViolations == 0 &&
                metrics.logicOwnerViolations == 0 &&
                metrics.endpointOwnerViolations == 0 &&
                metrics.connectionOwnerMigrations == 0 &&
                metrics.orderViolations == 0 && metrics.staleInputs == 0 &&
                metrics.staleOutputs == 0 && metrics.profileTerminalFailures == 0 &&
                metrics.logicCellCount == 2 && metrics.stableHashSelections == 1,
            .expectedHandoffModel = metrics.crossDomainHandoffs == 2,
        };
    }
    secondLogicThread.stop();
    firstLogicThread.stop();
    return observation;
}

}  // namespace

int main() {
    const auto profileA = runProfileA();
    const auto profileB = runProfileB();
    const auto profileC = runProfileC();
    const auto profileD = runProfileD();
    assertExpected("Profile A", profileA);
    assertExpected("Profile B", profileB);
    assertExpected("Profile C", profileC);
    assertExpected("Profile D", profileD);
    GAMENET_TEST_ASSERT(profileA == profileB);
    GAMENET_TEST_ASSERT(profileB == profileC);
    GAMENET_TEST_ASSERT(profileC == profileD);
    return 0;
}
