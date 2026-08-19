#include "runtime_profiles/MultiIoDedicatedFixedTick.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using gamenet::examples::FixedTickCadence;
using gamenet::examples::FixedTickContext;
using gamenet::examples::MultiIoDedicatedFixedTick;
using gamenet::examples::MultiIoDedicatedFixedTickAction;
using gamenet::examples::MultiIoDedicatedFixedTickHandlerResult;
using gamenet::examples::MultiIoDedicatedFixedTickMetrics;
using gamenet::examples::MultiIoDedicatedFixedTickOptions;
using gamenet::examples::MultiIoDedicatedFixedTickStopHandle;

MultiIoDedicatedFixedTickOptions profileOptions() {
    return {
        .ioThreads = 2,
        .framing = {
            .maxPayloadBytes = 512,
            .maxBufferedBytes = 4096,
            .maxFramesPerPush = 32,
            .maxFrameBytesPerPush = 4096,
            .maxRetainedCapacityBytes = 516,
            .trimThresholdBytes = 256,
        },
        .queueLimits = {
            .maxCommands = 64,
            .maxQueuedBytes = 64U * 512U,
            .maxPayloadBytes = 512,
        },
        .tickInterval = 20ms,
        .cadence = FixedTickCadence::FixedRateSkipMissed,
        .maxCatchUpTicks = 0,
        .maxCommandsPerTick = 2,
        .admission = {.maxConnections = 16},
        .connectionBackpressure = {
            .lowWaterMarkBytes = 1024,
            .highWaterMarkBytes = 2048,
            .hardLimitBytes = 4096,
            .maxInputBufferBytes = 4096,
        },
        .maxHandlerWallTime = 5s,
        .maxTickWallTime = 5s,
    };
}

std::string encodeFrames(
    const gamenet::protocol::PacketFramerOptions& options,
    const std::vector<std::string>& frames) {
    gamenet::protocol::PacketFramer encoder(options);
    std::string bytes;
    for (const auto& frame : frames) {
        const auto encoded = encoder.encode(frame);
        GAMENET_TEST_ASSERT(encoded.has_value());
        bytes += *encoded;
    }
    return bytes;
}

bool writeEventually(gamenet::net::SocketFd fd, std::string_view bytes) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (offset != bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        const int error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInProgress(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return offset == bytes.size();
}

bool readFramesEventually(
    gamenet::net::SocketFd fd,
    const gamenet::protocol::PacketFramerOptions& options,
    const std::vector<std::string>& expected) {
    gamenet::protocol::PacketFramer decoder(options);
    std::vector<std::string> received;
    std::array<char, 2048> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (received.size() < expected.size() &&
           std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
        if (count > 0) {
            auto result = decoder.push(
                std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (true) {
                received.insert(
                    received.end(),
                    std::make_move_iterator(result.frames.begin()),
                    std::make_move_iterator(result.frames.end()));
                if (!result.needsContinuation) break;
                result = decoder.push({});
            }
            continue;
        }
        if (count == 0) return false;
        const int error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return received == expected;
}

bool waitForClose(gamenet::net::SocketFd fd) {
    std::array<char, 64> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
        if (count == 0) return true;
        if (count < 0) {
            const int error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInterrupted(error)) {
                return true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::steady_clock::duration timeout = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void driveStop(
    gamenet::net::EventLoop& baseLoop,
    MultiIoDedicatedFixedTick& profile,
    std::atomic<bool>& clientDone,
    MultiIoDedicatedFixedTickStopHandle& stop) {
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (!clientDone.load(std::memory_order_acquire)) return;
        if (!stop.valid()) {
            stop = profile.stopGracefully({.drainTimeout = 0ms});
            return;
        }
        if (stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
}

void invalidConfigurationIsRejected() {
    gamenet::net::EventLoop baseLoop;
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    auto expectRejected = [&](
                              MultiIoDedicatedFixedTickOptions options,
                              gamenet::net::EventLoop* candidateLogicLoop) {
        bool rejected = false;
        try {
            MultiIoDedicatedFixedTick invalid(
                &baseLoop,
                candidateLogicLoop,
                gamenet::net::InetAddress(0, true),
                [](const FixedTickContext&, auto, auto) {
                    return MultiIoDedicatedFixedTickHandlerResult{};
                },
                options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);
    };

    auto options = profileOptions();
    options.ioThreads = 1;
    expectRejected(options, logicLoop);
    options = profileOptions();
    options.tickInterval = 0ms;
    expectRejected(options, logicLoop);
    options = profileOptions();
    options.maxCommandsPerTick = 0;
    expectRejected(options, logicLoop);
    options = profileOptions();
    options.maxCatchUpTicks = 1;
    expectRejected(options, logicLoop);
    options = profileOptions();
    options.cadence = FixedTickCadence::FixedRateBoundedCatchUp;
    options.maxCatchUpTicks = 0;
    expectRejected(options, logicLoop);
    expectRejected(profileOptions(), &baseLoop);
    logicThread.stop();
}

void fixedTickGateBoundsWorkAndPreservesOwners() {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    std::promise<void> releaseLogicPromise;
    auto releaseLogic = releaseLogicPromise.get_future().share();
    std::atomic<bool> logicBlocked{false};
    GAMENET_TEST_ASSERT(
        logicLoop->executor().post([&] {
            logicBlocked.store(true, std::memory_order_release);
            releaseLogic.wait();
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(waitUntil([&] { return logicBlocked.load(); }));

    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    std::mutex observationsMutex;
    std::vector<std::uint64_t> observedTicks;
    std::atomic<std::uint64_t> handlerCalls{0};
    auto profile = std::make_unique<MultiIoDedicatedFixedTick>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [&](const FixedTickContext& tick, auto, std::string_view payload) {
            handlerCalls.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(observationsMutex);
            observedTicks.push_back(tick.tickSequence);
            return MultiIoDedicatedFixedTickHandlerResult{
                .action = MultiIoDedicatedFixedTickAction::Continue,
                .reply = std::string(payload),
            };
        },
        options);
    profile->start();

    const std::vector<std::string> first{"a0", "a1", "a2"};
    const std::vector<std::string> second{"b0", "b1"};
    std::atomic<int> clientsDone{0};
    std::atomic<bool> clientsSucceeded{true};
    std::atomic<bool> networkOwnersReady{false};
    auto clientBody = [&](const std::vector<std::string>& expected) {
        const auto fd = gamenet::test::connectTestClient(profile->listenAddress());
        const bool ok = waitUntil([&] {
            return networkOwnersReady.load(std::memory_order_acquire);
        }) && writeEventually(fd, encodeFrames(options.framing, expected)) &&
            readFramesEventually(fd, options.framing, expected);
        if (!ok) clientsSucceeded.store(false);
        gamenet::test::closeTestSocket(fd);
        clientsDone.fetch_add(1, std::memory_order_release);
    };
    std::thread firstClient(clientBody, std::cref(first));
    std::thread secondClient(clientBody, std::cref(second));

    bool observedClosedTickGate = false;
    bool released = false;
    std::atomic<bool> clientDone{false};
    MultiIoDedicatedFixedTickStopHandle stop;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        const auto metrics = profile->metrics();
        if (metrics.connectionsOpened == 2 && metrics.networkOwnerCount == 2) {
            networkOwnersReady.store(true, std::memory_order_release);
        }
        if (!released && metrics.commandsAccepted == 5) {
            observedClosedTickGate = handlerCalls.load() == 0;
            released = true;
            releaseLogicPromise.set_value();
        }
        if (clientsDone.load(std::memory_order_acquire) == 2) {
            clientDone.store(true, std::memory_order_release);
        }
    }));
    static_cast<void>(baseLoop.runAfter(8s, [&] {
        if (!released) {
            released = true;
            releaseLogicPromise.set_value();
        }
    }));
    driveStop(baseLoop, *profile, clientDone, stop);
    gamenet::test::runLoopWithTimeout(baseLoop, 12s, "Profile C tick gate timed out");
    if (!released) releaseLogicPromise.set_value();
    firstClient.join();
    secondClient.join();

    GAMENET_TEST_ASSERT(clientsSucceeded.load());
    GAMENET_TEST_ASSERT(observedClosedTickGate);
    GAMENET_TEST_ASSERT(stop.networkStop.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(stop.logicStop.wait_for(0s) == std::future_status::ready);
    const MultiIoDedicatedFixedTickMetrics metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.networkOwnerCount == 2);
    GAMENET_TEST_ASSERT(metrics.commandsAccepted == 5);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 5);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 5);
    GAMENET_TEST_ASSERT(metrics.maxCommandsInTick == 2);
    GAMENET_TEST_ASSERT(metrics.ticksWithWork == 3);
    GAMENET_TEST_ASSERT(metrics.catchUpTicks == 0);
    GAMENET_TEST_ASSERT(metrics.crossDomainHandoffs == 10);
    GAMENET_TEST_ASSERT(metrics.networkOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.logicOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.endpointOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.tickJitterP999Us >= metrics.tickJitterP99Us);
    GAMENET_TEST_ASSERT(metrics.tickDurationP999Us >= metrics.tickDurationP99Us);
    GAMENET_TEST_ASSERT(metrics.queueAgeP999Us >= metrics.queueAgeP99Us);
    GAMENET_TEST_ASSERT(metrics.tickJitterP50Us > 0);
    GAMENET_TEST_ASSERT(metrics.tickDurationP99Us > 0);
    GAMENET_TEST_ASSERT(metrics.queueAgeP99Us > 0);
    GAMENET_TEST_ASSERT(metrics.timerSetupPosts == 1);
    GAMENET_TEST_ASSERT(metrics.timerSetupAccepted == 1);
    GAMENET_TEST_ASSERT(metrics.timerSetupFailures == 0);
    GAMENET_TEST_ASSERT(metrics.timerCancelPosts == 1);
    GAMENET_TEST_ASSERT(metrics.timerCancelAccepted == 1);
    {
        std::lock_guard lock(observationsMutex);
        std::map<std::uint64_t, std::size_t> commandsByTick;
        for (const auto tick : observedTicks) ++commandsByTick[tick];
        GAMENET_TEST_ASSERT(commandsByTick.size() == 3);
        auto current = commandsByTick.begin();
        GAMENET_TEST_ASSERT(current++->second == 2);
        GAMENET_TEST_ASSERT(current++->second == 2);
        GAMENET_TEST_ASSERT(current->second == 1);
    }
    profile.reset();
    logicThread.stop();
}

void deterministicOverrunBoundsCatchUpThenSkips() {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    options.tickInterval = 10ms;
    options.cadence = FixedTickCadence::FixedRateBoundedCatchUp;
    options.maxCatchUpTicks = 1;
    options.maxCommandsPerTick = 1;
    options.maxHandlerWallTime = 100ms;
    options.maxTickWallTime = 5ms;
    auto profile = std::make_unique<MultiIoDedicatedFixedTick>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [](const FixedTickContext&, auto, std::string_view payload) {
            std::this_thread::sleep_for(35ms);
            return MultiIoDedicatedFixedTickHandlerResult{
                .reply = std::string(payload),
            };
        },
        options);
    profile->start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(fd, encodeFrames(options.framing, {"slow"})) &&
            readFramesEventually(fd, options.framing, {"slow"});
        ok = ok && waitUntil([&] {
            const auto metrics = profile->metrics();
            return metrics.catchUpTicks >= 1 && metrics.skippedTicks >= 1;
        });
        gamenet::test::closeTestSocket(fd);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoDedicatedFixedTickStopHandle stop;
    driveStop(baseLoop, *profile, clientDone, stop);
    gamenet::test::runLoopWithTimeout(baseLoop, 8s, "Profile C overrun timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.catchUpTicks >= 1);
    GAMENET_TEST_ASSERT(metrics.skippedTicks >= 1);
    GAMENET_TEST_ASSERT(metrics.maxConsecutiveCatchUp == 1);
    GAMENET_TEST_ASSERT(metrics.tickOverruns >= 1);
    profile.reset();
    logicThread.stop();
}

void saturationClosesOneRouteAndFreshRouteRecovers() {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    std::promise<void> releaseLogicPromise;
    auto releaseLogic = releaseLogicPromise.get_future().share();
    std::atomic<bool> logicBlocked{false};
    GAMENET_TEST_ASSERT(
        logicLoop->executor().post([&] {
            logicBlocked.store(true, std::memory_order_release);
            releaseLogic.wait();
        }) == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(waitUntil([&] { return logicBlocked.load(); }));

    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    options.queueLimits.maxCommands = 2;
    options.queueLimits.maxQueuedBytes = 1024;
    options.maxCommandsPerTick = 1;
    auto profile = std::make_unique<MultiIoDedicatedFixedTick>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [](const FixedTickContext&, auto, std::string_view payload) {
            return MultiIoDedicatedFixedTickHandlerResult{
                .reply = std::string(payload),
            };
        },
        options);
    profile->start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto saturated = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(
            saturated,
            encodeFrames(options.framing, {"queued-1", "queued-2", "overflow"}));
        ok = ok && waitForClose(saturated);
        gamenet::test::closeTestSocket(saturated);
        ok = ok && waitUntil([&] {
            return profile->metrics().queueFullRejections >= 1;
        });
        releaseLogicPromise.set_value();
        ok = ok && waitUntil([&] {
            const auto metrics = profile->metrics();
            return metrics.queue.depth == 0 && metrics.staleInputs >= 2;
        });

        const auto recovered = gamenet::test::connectTestClient(profile->listenAddress());
        ok = ok && writeEventually(
            recovered, encodeFrames(options.framing, {"recovered"}));
        ok = ok && readFramesEventually(
            recovered, options.framing, {"recovered"});
        gamenet::test::closeTestSocket(recovered);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoDedicatedFixedTickStopHandle stop;
    driveStop(baseLoop, *profile, clientDone, stop);
    gamenet::test::runLoopWithTimeout(baseLoop, 10s, "Profile C recovery timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.queueFullRejections >= 1);
    GAMENET_TEST_ASSERT(metrics.staleInputs >= 2);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 1);
    GAMENET_TEST_ASSERT(metrics.queue.depth == 0);
    GAMENET_TEST_ASSERT(metrics.queue.depthHighWatermark == 2);
    profile.reset();
    logicThread.stop();
}

void activeHandlerDelaysLogicStopAndRevokesOutput() {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    options.maxCommandsPerTick = 1;
    std::promise<void> releaseHandlerPromise;
    auto releaseHandler = releaseHandlerPromise.get_future().share();
    std::atomic<bool> handlerEntered{false};
    auto profile = std::make_unique<MultiIoDedicatedFixedTick>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [&](const FixedTickContext&, auto, std::string_view) {
            handlerEntered.store(true, std::memory_order_release);
            releaseHandler.wait();
            return MultiIoDedicatedFixedTickHandlerResult{
                .reply = std::string("must-not-send"),
            };
        },
        options);
    profile->start();

    std::atomic<bool> stopIssued{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    bool logicFutureWasPending = false;
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(
            fd, encodeFrames(options.framing, {"hold", "cancel-me"})) &&
            waitUntil([&] { return stopIssued.load(std::memory_order_acquire); });
        releaseHandlerPromise.set_value();
        ok = ok && waitForClose(fd);
        gamenet::test::closeTestSocket(fd);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoDedicatedFixedTickStopHandle stop;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (handlerEntered.load(std::memory_order_acquire) && !stop.valid()) {
            stop = profile->stopGracefully({.drainTimeout = 0ms});
            logicFutureWasPending =
                stop.logicStop.wait_for(0s) != std::future_status::ready;
            stopIssued.store(true, std::memory_order_release);
            return;
        }
        if (!clientDone.load(std::memory_order_acquire) || !stop.valid()) return;
        if (stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
    gamenet::test::runLoopWithTimeout(baseLoop, 8s, "Profile C active stop timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    GAMENET_TEST_ASSERT(logicFutureWasPending);
    GAMENET_TEST_ASSERT(stop.logicStop.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(stop.networkStop.wait_for(0s) == std::future_status::ready);
    const auto summary = stop.logicStop.get();
    GAMENET_TEST_ASSERT(summary.droppedCommands >= 1);
    GAMENET_TEST_ASSERT(summary.activeTickObserved);
    GAMENET_TEST_ASSERT(!summary.terminalTimerFailure);
    GAMENET_TEST_ASSERT(
        summary.cadenceStopPostResult == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(summary.shutdownDrainWaitUs > 0);
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.staleOutputs >= 1);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 0);
    GAMENET_TEST_ASSERT(metrics.logicStopDroppedCommands >= 1);
    profile.reset();
    logicThread.stop();
}

}  // namespace

int main() {
    invalidConfigurationIsRejected();
    fixedTickGateBoundsWorkAndPreservesOwners();
    deterministicOverrunBoundsCatchUpThenSkips();
    saturationClosesOneRouteAndFreshRouteRecovers();
    activeHandlerDelaysLogicStopAndRevokesOutput();
    return 0;
}
