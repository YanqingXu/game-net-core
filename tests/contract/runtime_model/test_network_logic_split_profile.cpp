#include "runtime_profiles/MultiIoQueuedEvent.h"

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
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using gamenet::examples::MultiIoQueuedAction;
using gamenet::examples::MultiIoQueuedEvent;
using gamenet::examples::MultiIoQueuedEventMetrics;
using gamenet::examples::MultiIoQueuedEventOptions;
using gamenet::examples::MultiIoQueuedHandlerResult;
using gamenet::examples::MultiIoQueuedStopHandle;

MultiIoQueuedEventOptions profileOptions() {
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
        .maxCommandsPerDrain = 2,
        .admission = {.maxConnections = 16},
        .connectionBackpressure = {
            .lowWaterMarkBytes = 1024,
            .highWaterMarkBytes = 2048,
            .hardLimitBytes = 4096,
            .maxInputBufferBytes = 4096,
        },
        .maxHandlerWallTime = 5s,
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
    const auto deadline = std::chrono::steady_clock::now() + 4s;
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
    const auto deadline = std::chrono::steady_clock::now() + 4s;
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
bool waitUntil(Predicate predicate, std::chrono::steady_clock::duration timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void waitForStopAndQuit(
    gamenet::net::EventLoop& baseLoop,
    MultiIoQueuedStopHandle& stop,
    std::atomic<bool>& clientDone,
    MultiIoQueuedEvent& profile) {
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

    auto expectRejected = [&](MultiIoQueuedEventOptions options, gamenet::net::EventLoop* logic) {
        bool rejected = false;
        try {
            MultiIoQueuedEvent invalid(
                &baseLoop,
                logic,
                gamenet::net::InetAddress(0, true),
                [](auto, auto) { return MultiIoQueuedHandlerResult{}; },
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
    options.maxCommandsPerDrain = 0;
    expectRejected(options, logicLoop);
    expectRejected(profileOptions(), &baseLoop);
    logicThread.stop();
}

void boundedCoalescedHandoffPreservesOwnersAndOrder() {
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
    auto profile = std::make_unique<MultiIoQueuedEvent>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [](auto, std::string_view payload) {
            return MultiIoQueuedHandlerResult{
                .action = MultiIoQueuedAction::Continue,
                .reply = std::string(payload),
            };
        },
        options);
    profile->start();

    const std::vector<std::string> first{"a0", "a1", "a2", "a3", "a4"};
    const std::vector<std::string> second{"b0", "b1", "b2", "b3", "b4"};
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

    bool released = false;
    bool watchdogTriggered = false;
    std::atomic<bool> clientDone{false};
    MultiIoQueuedStopHandle stop;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        const auto metrics = profile->metrics();
        if (metrics.connectionsOpened == 2 && metrics.networkOwnerCount == 2) {
            networkOwnersReady.store(true, std::memory_order_release);
        }
        if (!released && metrics.commandsAccepted == 10) {
            released = true;
            releaseLogicPromise.set_value();
        }
        if (clientsDone.load(std::memory_order_acquire) == 2) {
            clientDone.store(true, std::memory_order_release);
        }
    }));
    static_cast<void>(baseLoop.runAfter(7s, [&] {
        watchdogTriggered = true;
        const auto snapshot = profile->metrics();
        std::cerr << "Profile B ordered watchdog: accepted="
                  << snapshot.commandsAccepted << " clientsDone="
                  << clientsDone.load() << " released=" << released
                  << " stopValid=" << stop.valid() << '\n';
        if (!released) {
            released = true;
            releaseLogicPromise.set_value();
        }
    }));
    waitForStopAndQuit(baseLoop, stop, clientDone, *profile);
    gamenet::test::runLoopWithTimeout(baseLoop, 14s, "Profile B ordered handoff timed out");
    if (!released) releaseLogicPromise.set_value();
    firstClient.join();
    secondClient.join();

    GAMENET_TEST_ASSERT(clientsSucceeded.load());
    GAMENET_TEST_ASSERT(!watchdogTriggered);
    GAMENET_TEST_ASSERT(stop.valid());
    GAMENET_TEST_ASSERT(stop.networkStop.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(stop.logicStop.wait_for(0s) == std::future_status::ready);
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.connectionsOpened == 2);
    GAMENET_TEST_ASSERT(metrics.connectionsClosed == 2);
    GAMENET_TEST_ASSERT(metrics.networkOwnerCount == 2);
    GAMENET_TEST_ASSERT(metrics.commandsAccepted == 10);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 10);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 10);
    GAMENET_TEST_ASSERT(metrics.producerWakePosts == 1);
    GAMENET_TEST_ASSERT(metrics.producerWakeMerges == 9);
    GAMENET_TEST_ASSERT(metrics.maxCommandsInDrain == 2);
    GAMENET_TEST_ASSERT(metrics.drainContinuations == 4);
    GAMENET_TEST_ASSERT(metrics.crossDomainHandoffs == 20);
    GAMENET_TEST_ASSERT(metrics.networkOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.logicOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.endpointOwnerViolations == 0);
    GAMENET_TEST_ASSERT(metrics.networkToLogicP99Us > 0);
    GAMENET_TEST_ASSERT(metrics.networkToLogicP999Us >= metrics.networkToLogicP99Us);
    GAMENET_TEST_ASSERT(metrics.logicToNetworkP999Us >= metrics.logicToNetworkP99Us);
    GAMENET_TEST_ASSERT(metrics.queueOldestAgeMaxUs >= metrics.networkToLogicP999Us);
    profile.reset();
    logicThread.stop();
}

void slowConsumerSaturatesThenRecoversWithoutStaleOutput() {
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    gamenet::net::EventLoop baseLoop;
    auto options = profileOptions();
    options.queueLimits.maxCommands = 2;
    options.queueLimits.maxQueuedBytes = 1024;
    options.maxCommandsPerDrain = 1;

    std::promise<void> releaseSlowPromise;
    auto releaseSlow = releaseSlowPromise.get_future().share();
    std::atomic<bool> slowEntered{false};
    auto profile = std::make_unique<MultiIoQueuedEvent>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [&](auto, std::string_view payload) {
            if (payload == "slow") {
                slowEntered.store(true, std::memory_order_release);
                releaseSlow.wait();
            }
            return MultiIoQueuedHandlerResult{.reply = std::string(payload)};
        },
        options);
    profile->start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto first = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(first, encodeFrames(options.framing, {"slow"})) &&
            waitUntil([&] { return slowEntered.load(std::memory_order_acquire); });
        ok = ok && writeEventually(
            first,
            encodeFrames(options.framing, {"queued-1", "queued-2", "overflow"}));
        ok = ok && waitForClose(first);
        gamenet::test::closeTestSocket(first);
        releaseSlowPromise.set_value();
        ok = ok && waitUntil([&] {
            const auto snapshot = profile->metrics();
            return snapshot.queue.depth == 0 && snapshot.staleInputs >= 2 &&
                snapshot.staleOutputs >= 1;
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

    MultiIoQueuedStopHandle stop;
    waitForStopAndQuit(baseLoop, stop, clientDone, *profile);
    gamenet::test::runLoopWithTimeout(baseLoop, 10s, "Profile B recovery timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.queueFullRejections >= 1);
    GAMENET_TEST_ASSERT(metrics.staleInputs >= 2);
    GAMENET_TEST_ASSERT(metrics.staleOutputs >= 1);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 2);
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
    std::promise<void> releaseHandlerPromise;
    auto releaseHandler = releaseHandlerPromise.get_future().share();
    std::atomic<bool> handlerEntered{false};

    auto profile = std::make_unique<MultiIoQueuedEvent>(
        &baseLoop,
        logicLoop,
        gamenet::net::InetAddress(0, true),
        [&](auto, std::string_view) {
            handlerEntered.store(true, std::memory_order_release);
            releaseHandler.wait();
            return MultiIoQueuedHandlerResult{.reply = std::string("must-not-send")};
        },
        options);
    profile->start();

    std::atomic<bool> stopIssued{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(profile->listenAddress());
        bool ok = writeEventually(fd, encodeFrames(options.framing, {"hold"})) &&
            waitUntil([&] { return stopIssued.load(std::memory_order_acquire); });
        releaseHandlerPromise.set_value();
        ok = ok && waitForClose(fd);
        gamenet::test::closeTestSocket(fd);
        clientSucceeded.store(ok);
        clientDone.store(true, std::memory_order_release);
    });

    MultiIoQueuedStopHandle stop;
    static_cast<void>(baseLoop.runEvery(1ms, [&] {
        if (handlerEntered.load(std::memory_order_acquire) && !stop.valid()) {
            stop = profile->stopGracefully({.drainTimeout = 0ms});
            GAMENET_TEST_ASSERT(
                stop.logicStop.wait_for(0s) != std::future_status::ready);
            stopIssued.store(true, std::memory_order_release);
            return;
        }
        if (!clientDone.load(std::memory_order_acquire) || !stop.valid()) return;
        if (stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready) {
            baseLoop.quit();
        }
    }));
    gamenet::test::runLoopWithTimeout(baseLoop, 8s, "Profile B active stop timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    GAMENET_TEST_ASSERT(stop.logicStop.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(stop.networkStop.wait_for(0s) == std::future_status::ready);
    const auto metrics = profile->metrics();
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.staleOutputs >= 1);
    GAMENET_TEST_ASSERT(metrics.outputsAccepted == 0);
    profile.reset();
    logicThread.stop();
}

}  // namespace

int main() {
    invalidConfigurationIsRejected();
    boundedCoalescedHandoffPreservesOwnersAndOrder();
    slowConsumerSaturatesThenRecoversWithoutStaleOutput();
    activeHandlerDelaysLogicStopAndRevokesOutput();
    return 0;
}
