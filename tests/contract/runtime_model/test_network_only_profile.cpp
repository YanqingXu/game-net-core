#include "runtime_profiles/SingleLoopInlineEvent.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using gamenet::examples::SingleLoopInlineAction;
using gamenet::examples::SingleLoopInlineEvent;
using gamenet::examples::SingleLoopInlineEventMetrics;
using gamenet::examples::SingleLoopInlineEventOptions;
using gamenet::examples::SingleLoopInlineHandler;
using gamenet::examples::SingleLoopInlineHandlerResult;

SingleLoopInlineEventOptions profileOptions(std::size_t framesPerDispatch = 2) {
    return {
        .framing = {
            .maxPayloadBytes = 512,
            .maxBufferedBytes = 4096,
            .maxFramesPerPush = framesPerDispatch,
            .maxFrameBytesPerPush = 2048,
            .maxRetainedCapacityBytes = 516,
            .trimThresholdBytes = 256,
        },
        .admission = {.maxConnections = 8},
        .connectionBackpressure = {
            .lowWaterMarkBytes = 1024,
            .highWaterMarkBytes = 2048,
            .hardLimitBytes = 4096,
            .maxInputBufferBytes = 4096,
        },
        .maxHandlerWallTime = 100ms,
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
    const auto deadline = std::chrono::steady_clock::now() + 2s;
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
    std::array<char, 1024> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (received.size() < expected.size() &&
           std::chrono::steady_clock::now() < deadline) {
        const auto count =
            gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
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
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count =
            gamenet::net::sockets::read(fd, bytes.data(), bytes.size());
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

template <typename ClientOperation>
SingleLoopInlineEventMetrics runScenario(
    gamenet::net::EventLoopOptions loopOptions,
    SingleLoopInlineEventOptions options,
    SingleLoopInlineHandler handler,
    ClientOperation clientOperation,
    gamenet::net::EventLoop** loopObserver = nullptr,
    SingleLoopInlineEvent** profileObserver = nullptr) {
    gamenet::net::EventLoop loop(loopOptions);
    if (loopObserver != nullptr) *loopObserver = &loop;
    SingleLoopInlineEvent profile(
        &loop,
        gamenet::net::InetAddress(0, true),
        std::move(handler),
        options);
    if (profileObserver != nullptr) *profileObserver = &profile;
    profile.start();

    std::atomic<bool> clientDone{false};
    std::atomic<bool> clientSucceeded{false};
    gamenet::net::TcpServerStopFuture stopFuture;
    bool stopRequested = false;
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(profile.listenAddress());
        clientSucceeded.store(clientOperation(fd, options.framing));
        gamenet::test::closeTestSocket(fd);
        clientDone.store(true, std::memory_order_release);
    });

    loop.runEvery(1ms, [&] {
        if (!clientDone.load(std::memory_order_acquire)) return;
        if (!stopRequested) {
            stopFuture = profile.stopGracefully(
                {.drainTimeout = std::chrono::milliseconds::zero()});
            stopRequested = true;
            return;
        }
        if (stopFuture.wait_for(0s) == std::future_status::ready) {
            loop.quit();
        }
    });
    gamenet::test::runLoopWithTimeout(
        loop, 5s, "SingleLoopInlineEvent scenario timed out");
    client.join();

    GAMENET_TEST_ASSERT(clientSucceeded.load());
    stopFuture = profile.stopGracefully();
    GAMENET_TEST_ASSERT(stopFuture.valid());
    GAMENET_TEST_ASSERT(stopFuture.wait_for(0s) == std::future_status::ready);
    const auto metrics = profile.metrics();
    GAMENET_TEST_ASSERT(metrics.connectionsOpened == 1);
    GAMENET_TEST_ASSERT(metrics.connectionsClosed == 1);
    return metrics;
}

void invalidConfigurationIsRejected() {
    gamenet::net::EventLoop loop;
    auto options = profileOptions();
    options.maxHandlerWallTime = std::chrono::steady_clock::duration::zero();
    bool rejected = false;
    try {
        SingleLoopInlineEvent invalid(
            &loop,
            gamenet::net::InetAddress(0, true),
            [](auto, auto) { return SingleLoopInlineHandlerResult{}; },
            options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GAMENET_TEST_ASSERT(rejected);
}

void boundedInlineEchoPreservesOwnerAndOrder() {
    gamenet::net::EventLoop* ownerLoop = nullptr;
    const std::thread::id ownerThread = std::this_thread::get_id();
    std::atomic<bool> wrongOwner{false};
    std::vector<std::string> expected{"one", "two", "three", "four", "five"};

    gamenet::net::EventLoopOptions loopOptions;
    auto options = profileOptions(2);
    const auto metrics = runScenario(
        loopOptions,
        options,
        [&](auto, std::string_view payload) {
            wrongOwner.store(
                ownerLoop == nullptr || !ownerLoop->isInLoopThread() ||
                std::this_thread::get_id() != ownerThread);
            return SingleLoopInlineHandlerResult{
                .action = SingleLoopInlineAction::Continue,
                .reply = std::string(payload),
            };
        },
        [&](auto fd, const auto& framing) {
            return writeEventually(fd, encodeFrames(framing, expected)) &&
                readFramesEventually(fd, framing, expected);
        },
        &ownerLoop);

    GAMENET_TEST_ASSERT(!wrongOwner.load());
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 5);
    GAMENET_TEST_ASSERT(metrics.repliesAccepted == 5);
    GAMENET_TEST_ASSERT(metrics.maxHandlersPerDispatch == 2);
    GAMENET_TEST_ASSERT(metrics.continuationPosts == 2);
    GAMENET_TEST_ASSERT(metrics.continuationRejections == 0);
    GAMENET_TEST_ASSERT(metrics.crossDomainHandoffs == 0);
    GAMENET_TEST_ASSERT(metrics.handlerOverruns == 0);
    GAMENET_TEST_ASSERT(metrics.outputOverloads == 0);
}

void saturatedContinuationFailsClosedWithoutRecursion() {
    gamenet::net::EventLoop* loop = nullptr;
    std::size_t handlerCalls = 0;
    gamenet::net::EventLoopOptions loopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    };
    auto options = profileOptions(1);
    const std::vector<std::string> input{"first", "must-not-run"};
    const auto metrics = runScenario(
        loopOptions,
        options,
        [&](auto, std::string_view) {
            ++handlerCalls;
            GAMENET_TEST_ASSERT(loop != nullptr);
            GAMENET_TEST_ASSERT(loop->tryQueueInLoop([] {}));
            return SingleLoopInlineHandlerResult{};
        },
        [&](auto fd, const auto& framing) {
            return writeEventually(fd, encodeFrames(framing, input)) &&
                waitForClose(fd);
        },
        &loop);
    GAMENET_TEST_ASSERT(handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.continuationPosts == 0);
    GAMENET_TEST_ASSERT(metrics.continuationRejections == 1);
}

void blockingViolationStopsBeforeNextFrame() {
    auto options = profileOptions(4);
    options.maxHandlerWallTime = 2ms;
    std::size_t handlerCalls = 0;
    const std::vector<std::string> input{"slow", "must-not-run"};
    const auto metrics = runScenario(
        {},
        options,
        [&](auto, std::string_view) {
            ++handlerCalls;
            std::this_thread::sleep_for(15ms);
            return SingleLoopInlineHandlerResult{};
        },
        [&](auto fd, const auto& framing) {
            return writeEventually(fd, encodeFrames(framing, input)) &&
                waitForClose(fd);
        });
    GAMENET_TEST_ASSERT(handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.handlerOverruns == 1);
}

void outputOverloadIsTerminal() {
    auto options = profileOptions(1);
    options.connectionBackpressure = {
        .lowWaterMarkBytes = 16,
        .highWaterMarkBytes = 32,
        .hardLimitBytes = 64,
        .maxInputBufferBytes = 4096,
    };
    const auto metrics = runScenario(
        {},
        options,
        [](auto, std::string_view) {
            return SingleLoopInlineHandlerResult{
                .reply = std::string(256, 'x'),
            };
        },
        [&](auto fd, const auto& framing) {
            return writeEventually(fd, encodeFrames(framing, {"request"})) &&
                waitForClose(fd);
        });
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.repliesAccepted == 0);
    GAMENET_TEST_ASSERT(metrics.outputOverloads == 1);
}

void callbackReentrantStopRevokesReply() {
    SingleLoopInlineEvent* reentrantProfile = nullptr;
    const auto options = profileOptions(1);
    const auto metrics = runScenario(
        {},
        options,
        [&](auto, std::string_view) {
            GAMENET_TEST_ASSERT(reentrantProfile != nullptr);
            reentrantProfile->stop();
            return SingleLoopInlineHandlerResult{
                .reply = std::string("must-not-escape-stop"),
            };
        },
        [&](auto fd, const auto& framing) {
            return writeEventually(fd, encodeFrames(framing, {"stop"})) &&
                waitForClose(fd);
        },
        nullptr,
        &reentrantProfile);
    GAMENET_TEST_ASSERT(metrics.handlerCalls == 1);
    GAMENET_TEST_ASSERT(metrics.repliesAccepted == 0);
}

}  // namespace

int main() {
    invalidConfigurationIsRejected();
    boundedInlineEchoPreservesOwnerAndOrder();
    saturatedContinuationFailsClosedWithoutRecursion();
    blockingViolationStopsBeforeNextFrame();
    outputOverloadIsTerminal();
    callbackReentrantStopRevokesReply();
    return 0;
}
