#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopControlRegistry.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using ActiveHarness = gamenet::net::detail::EventLoopActiveBatchHarness;
using ControlRegistry = gamenet::net::detail::EventLoopControlRegistry;

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
        gamenet::net::sockets::createSocketPairOrDie(fds);
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

}  // namespace

int main() {
    testActiveBatchContinuationAndBetweenRoundInvalidation();
    testExpiredTimerBudgetYieldsToAcceptedFunctor();
    testControlBudgetYieldsToTimerAndFunctor();
    return 0;
}
