#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopControlRegistry.h"
#include "../../../src/core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

namespace {

using ActiveHarness = gamenet::net::detail::EventLoopActiveBatchHarness;
using ControlRegistry = gamenet::net::detail::EventLoopControlRegistry;
using LifecycleRegistry = gamenet::net::detail::EventLoopLifecycleRegistry;

bool rejectsOptions(gamenet::net::EventLoopOptions options) {
    try {
        gamenet::net::EventLoop loop(options);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

struct SocketPair {
    gamenet::net::SocketFd first{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd second{gamenet::net::kInvalidSocket};

    SocketPair() {
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
#ifdef _WIN32
        gamenet::net::sockets::createSocketPairOrDie(fds);
#else
        GAMENET_TEST_ASSERT(
            ::socketpair(
                AF_UNIX,
                SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                0,
                fds) == 0);
#endif
        first = fds[0];
        second = fds[1];
    }

    ~SocketPair() {
        gamenet::net::sockets::close(first);
        gamenet::net::sockets::close(second);
    }
};

void removeChannel(std::unique_ptr<gamenet::net::Channel>& channel) {
    if (!channel) {
        return;
    }
    channel->disableAll();
    channel->remove();
    channel.reset();
}

void testActiveBatchContinuationAndBetweenRoundInvalidation() {
    GAMENET_TEST_ASSERT(rejectsOptions(
        gamenet::net::EventLoopOptions{
            .maxActiveChannelsPerIteration = 0,
        }));
    GAMENET_TEST_ASSERT(rejectsOptions(
        gamenet::net::EventLoopOptions{
            .maxTimersPerIteration = 0,
        }));
    GAMENET_TEST_ASSERT(rejectsOptions(
        gamenet::net::EventLoopOptions{
            .maxControlCallbacksPerIteration = 0,
        }));
    GAMENET_TEST_ASSERT(rejectsOptions(
        gamenet::net::EventLoopOptions{
            .maxIocpCompletionsPerPoll = 0,
        }));
    GAMENET_TEST_ASSERT(rejectsOptions(
        gamenet::net::EventLoopOptions{
            .maxIocpCompletionsPerPoll = 65,
        }));

    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxActiveChannelsPerIteration = 1,
    });
    SocketPair firstPair;
    SocketPair secondPair;
    SocketPair thirdPair;
    auto first = std::make_unique<gamenet::net::Channel>(&loop, firstPair.first);
    auto second = std::make_unique<gamenet::net::Channel>(&loop, secondPair.first);
    auto third = std::make_unique<gamenet::net::Channel>(&loop, thirdPair.first);
    std::vector<int> order;
    std::vector<gamenet::net::EventLoopMetricSample> samples;

    first->setReadCallback([&](gamenet::base::Timestamp) { order.push_back(1); });
    second->setReadCallback([&](gamenet::base::Timestamp) { order.push_back(2); });
    third->setReadCallback([&](gamenet::base::Timestamp) { order.push_back(3); });
    for (auto* channel : {first.get(), second.get(), third.get()}) {
        channel->enableReading();
        channel->setRevents(gamenet::net::Channel::kReadEvent);
    }
    loop.setEventLoopMetricCallback(
        [&](const gamenet::net::EventLoopMetricSample& sample) {
            if (sample.event ==
                gamenet::net::EventLoopMetricEvent::ActiveChannelsDrained) {
                samples.push_back(sample);
            }
        });

    ActiveHarness::install(
        loop,
        {first.get(), second.get(), third.get()},
        gamenet::base::now());
    ActiveHarness::dispatchRound(loop);
    GAMENET_TEST_ASSERT((order == std::vector<int>{1}));
    GAMENET_TEST_ASSERT(ActiveHarness::pendingCount(loop) == 2);

    // The batch stays loop-owned between rounds. A timer/control phase may
    // remove and destroy a not-yet-dispatched Channel without leaving a raw
    // observation in the continuation.
    removeChannel(third);
    ActiveHarness::dispatchRound(loop);
    GAMENET_TEST_ASSERT((order == std::vector<int>{1, 2}));
    GAMENET_TEST_ASSERT(ActiveHarness::pendingCount(loop) == 0);

    GAMENET_TEST_ASSERT(samples.size() == 2);
    GAMENET_TEST_ASSERT(samples[0].drainedWork == 1);
    GAMENET_TEST_ASSERT(samples[0].remainingWork == 2);
    GAMENET_TEST_ASSERT(samples[0].budgetExhausted);
    GAMENET_TEST_ASSERT(samples[1].drainedWork == 1);
    GAMENET_TEST_ASSERT(samples[1].remainingWork == 0);
    GAMENET_TEST_ASSERT(!samples[1].budgetExhausted);

    removeChannel(first);
    removeChannel(second);
}

void testExpiredTimerBudgetYieldsToAcceptedFunctor() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxTimersPerIteration = 1,
    });
    std::vector<int> order;
    std::vector<gamenet::net::EventLoopMetricSample> samples;
    loop.setEventLoopMetricCallback(
        [&](const gamenet::net::EventLoopMetricSample& sample) {
            if (sample.event ==
                gamenet::net::EventLoopMetricEvent::TimersDrained) {
                samples.push_back(sample);
            }
        });

    const auto ready = gamenet::base::now() - 1ms;
    loop.runAt(ready, [&] { order.push_back(1); });
    loop.runAt(ready, [&] { order.push_back(2); });
    loop.runAt(ready, [&] {
        order.push_back(3);
        loop.quit();
    });
    GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] { order.push_back(10); }));
    loop.loop();

    GAMENET_TEST_ASSERT((order == std::vector<int>{1, 10, 2, 3}));
    GAMENET_TEST_ASSERT(samples.size() == 3);
    GAMENET_TEST_ASSERT(samples[0].remainingWork == 2);
    GAMENET_TEST_ASSERT(samples[1].remainingWork == 1);
    GAMENET_TEST_ASSERT(samples[2].remainingWork == 0);
    GAMENET_TEST_ASSERT(samples[0].budgetExhausted);
    GAMENET_TEST_ASSERT(!samples[2].budgetExhausted);
    GAMENET_TEST_ASSERT(samples[0].oldestReadyLatency >= 1ms);
}

void testControlBudgetYieldsToTimerAndFunctor() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 3,
        .maxControlCallbacksPerIteration = 1,
    });
    std::vector<int> order;
    std::vector<gamenet::net::EventLoopMetricSample> samples;
    loop.setEventLoopMetricCallback(
        [&](const gamenet::net::EventLoopMetricSample& sample) {
            if (sample.event ==
                gamenet::net::EventLoopMetricEvent::ControlSourcesDrained) {
                samples.push_back(sample);
            }
        });

    auto first = ControlRegistry::registerSource(loop, [&] { order.push_back(1); });
    auto second = ControlRegistry::registerSource(loop, [&] { order.push_back(2); });
    auto third = ControlRegistry::registerSource(loop, [&] {
        order.push_back(3);
        loop.quit();
    });
    GAMENET_TEST_ASSERT(first.notify() == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(second.notify() == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(third.notify() == gamenet::net::PostResult::Accepted);
    loop.runAfter(0ms, [&] { order.push_back(10); });
    GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] { order.push_back(20); }));
    loop.loop();

    GAMENET_TEST_ASSERT(
        (order == std::vector<int>{10, 1, 20, 2, 3}));
    GAMENET_TEST_ASSERT(samples.size() == 3);
    GAMENET_TEST_ASSERT(samples[0].remainingWork == 2);
    GAMENET_TEST_ASSERT(samples[1].remainingWork == 1);
    GAMENET_TEST_ASSERT(samples[2].remainingWork == 0);
    GAMENET_TEST_ASSERT(samples[0].budgetExhausted);
    GAMENET_TEST_ASSERT(!samples[2].budgetExhausted);

    ControlRegistry::unregisterSource(loop, first);
    ControlRegistry::unregisterSource(loop, second);
    ControlRegistry::unregisterSource(loop, third);
}

void testSustainedSourcesReceiveOneServicePerRound() {
    constexpr int kRounds = 12;
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 1,
        .maxLifecycleNodes = 1,
        .maxLifecycleCallbacksPerIteration = 1,
        .maxActiveChannelsPerIteration = 1,
        .maxTimersPerIteration = 1,
        .maxControlCallbacksPerIteration = 1,
        .maxIocpCompletionsPerPoll = 1,
    });
    gamenet::net::Channel channel(
        &loop,
        gamenet::net::kInvalidSocket);
    std::vector<std::string_view> order;
    int ioCalls = 0;
    int timerCalls = 0;
    int controlCalls = 0;
    int lifecycleCalls = 0;
    int functorCalls = 0;
    int callbackDepth = 0;
    int callbackDepthPeak = 0;

    auto enter = [&] {
        ++callbackDepth;
        callbackDepthPeak =
            (std::max)(callbackDepthPeak, callbackDepth);
    };
    auto leave = [&] { --callbackDepth; };

    channel.setReadCallback([&](gamenet::base::Timestamp) {
        enter();
        order.push_back("io");
        ++ioCalls;
        leave();
    });

    std::function<void()> timerCallback;
    timerCallback = [&] {
        enter();
        order.push_back("timer");
        ++timerCalls;
        if (timerCalls < kRounds) {
            loop.runAfter(0ms, timerCallback);
        }
        leave();
    };

    gamenet::net::EventLoopControlSource control;
    control = ControlRegistry::registerSource(loop, [&] {
        enter();
        order.push_back("control");
        ++controlCalls;
        if (controlCalls < kRounds) {
            GAMENET_TEST_ASSERT(
                control.notify() == gamenet::net::PostResult::Accepted);
        }
        leave();
    });

    gamenet::net::EventLoopLifecycleSource lifecycle;
    lifecycle = LifecycleRegistry::attach(loop, [&] {
        enter();
        order.push_back("lifecycle");
        ++lifecycleCalls;
        if (lifecycleCalls < kRounds) {
            GAMENET_TEST_ASSERT(
                lifecycle.signal() ==
                gamenet::net::PostResult::Accepted);
        }
        leave();
    });

    std::function<void()> functor;
    functor = [&] {
        enter();
        order.push_back("functor");
        ++functorCalls;
        if (functorCalls < kRounds) {
            GAMENET_TEST_ASSERT(loop.tryQueueInLoop(functor));
        }
        leave();
    };

    loop.runAfter(0ms, timerCallback);
    GAMENET_TEST_ASSERT(
        control.notify() == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        lifecycle.signal() == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(loop.tryQueueInLoop(functor));

    for (int round = 0; round < kRounds; ++round) {
        channel.setRevents(gamenet::net::Channel::kReadEvent);
        ActiveHarness::runFairRound(
            loop,
            {&channel},
            gamenet::base::now());
        GAMENET_TEST_ASSERT(ioCalls == round + 1);
        GAMENET_TEST_ASSERT(timerCalls == round + 1);
        GAMENET_TEST_ASSERT(controlCalls == round + 1);
        GAMENET_TEST_ASSERT(lifecycleCalls == round + 1);
        GAMENET_TEST_ASSERT(functorCalls == round + 1);
        const auto offset = static_cast<std::size_t>(round) * 5;
        GAMENET_TEST_ASSERT(order[offset] == "io");
        GAMENET_TEST_ASSERT(order[offset + 1] == "timer");
        GAMENET_TEST_ASSERT(order[offset + 2] == "control");
        GAMENET_TEST_ASSERT(order[offset + 3] == "lifecycle");
        GAMENET_TEST_ASSERT(order[offset + 4] == "functor");
    }

    GAMENET_TEST_ASSERT(callbackDepth == 0);
    GAMENET_TEST_ASSERT(callbackDepthPeak == 1);
    GAMENET_TEST_ASSERT(loop.pendingControlSourceCount() == 0);
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 0);
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 0);
    ControlRegistry::unregisterSource(loop, control);
    LifecycleRegistry::detach(loop, lifecycle);
}

}  // namespace

int main() {
    testActiveBatchContinuationAndBetweenRoundInvalidation();
    testExpiredTimerBudgetYieldsToAcceptedFunctor();
    testControlBudgetYieldsToTimerAndFunctor();
    testSustainedSourcesReceiveOneServicePerRound();
    return 0;
}
