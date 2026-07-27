#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/PostResult.h"

#include "support/TestAssert.h"

#include "../../../src/core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

using LifecycleRegistry =
    gamenet::net::detail::EventLoopLifecycleRegistry;

void testCommittedNotifySurvivesDetachAndSaturation() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 0,
        .maxLifecycleNodes = 1,
        .maxLifecycleCallbacksPerIteration = 1,
    });

    int callbackCalls = 0;
    auto source = LifecycleRegistry::attach(loop, [&] {
        ++callbackCalls;
    });

    GAMENET_TEST_ASSERT(loop.tryQueueInLoop([] {}));
    loop.queueInLoop([] {});
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    for (int index = 0; index < 10'000; ++index) {
        GAMENET_TEST_ASSERT(
            source.signal() == gamenet::net::PostResult::Accepted);
    }
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 1);
    GAMENET_TEST_ASSERT(loop.mergedLifecycleSignalCount() == 9'999);

    // lifecycle-committed-notify: detach invalidates later callers, but it
    // cannot revoke a signal that returned Accepted for the old generation.
    LifecycleRegistry::detach(loop, source);
    GAMENET_TEST_ASSERT(
        source.signal() == gamenet::net::PostResult::OwnerUnavailable);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);

    loop.runAfter(0ms, [&] { loop.quit(); });
    loop.loop();

    GAMENET_TEST_ASSERT(callbackCalls == 1);
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 0);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);
}

void testLifecycleCapacityGenerationAndBudgetedSelfSignal() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 0,
        .maxLifecycleNodes = 1,
        .maxLifecycleCallbacksPerIteration = 1,
    });

    int oldCalls = 0;
    auto oldSource = LifecycleRegistry::attach(loop, [&] { ++oldCalls; });

    bool capacityFailureWasExplicit = false;
    try {
        (void)LifecycleRegistry::attach(loop, [] {});
    } catch (const std::length_error&) {
        capacityFailureWasExplicit = true;
    }
    GAMENET_TEST_ASSERT(capacityFailureWasExplicit);

    LifecycleRegistry::detach(loop, oldSource);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);

    int callbackDepth = 0;
    int callbackDepthPeak = 0;
    int newCalls = 0;
    gamenet::net::EventLoopLifecycleSource newSource;
    newSource = LifecycleRegistry::attach(loop, [&] {
        ++callbackDepth;
        callbackDepthPeak = (std::max)(callbackDepthPeak, callbackDepth);
        ++newCalls;
        if (newCalls == 1) {
            GAMENET_TEST_ASSERT(
                newSource.signal() == gamenet::net::PostResult::Accepted);
        } else {
            loop.quit();
        }
        --callbackDepth;
    });

    // lifecycle-detach-generation: a copied handle for the detached node
    // cannot dirty the replacement that consumed the released capacity.
    GAMENET_TEST_ASSERT(
        oldSource.signal() == gamenet::net::PostResult::OwnerUnavailable);
    GAMENET_TEST_ASSERT(
        newSource.signal() == gamenet::net::PostResult::Accepted);

    loop.loop();

    GAMENET_TEST_ASSERT(oldCalls == 0);
    GAMENET_TEST_ASSERT(newCalls == 2);
    GAMENET_TEST_ASSERT(callbackDepthPeak == 1);
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 0);

    LifecycleRegistry::detach(loop, newSource);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
}

void testLifecycleNotifyQuitLinearization() {
    constexpr int repetitions = 100;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        std::promise<gamenet::net::EventLoop*> loopPromise;
        auto loopFuture = loopPromise.get_future();
        std::promise<gamenet::net::EventLoopLifecycleSource> sourcePromise;
        auto sourceFuture = sourcePromise.get_future();
        std::promise<void> operationsDonePromise;
        auto operationsDoneFuture = operationsDonePromise.get_future();
        std::atomic<int> executed{0};

        std::thread owner([&] {
            gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
                .maxPendingFunctors = 1,
                .reservedPendingFunctors = 0,
                .maxFunctorsPerIteration = 1,
                .maxControlSources = 0,
                .maxLifecycleNodes = 1,
                .maxLifecycleCallbacksPerIteration = 1,
            });
            auto source =
                LifecycleRegistry::attach(loop, [&] { ++executed; });
            loopPromise.set_value(&loop);
            sourcePromise.set_value(source);
            loop.loop();
            operationsDoneFuture.wait();
            LifecycleRegistry::detach(loop, source);
        });

        gamenet::net::EventLoop* loop = loopFuture.get();
        auto source = sourceFuture.get();
        std::atomic<bool> start{false};
        gamenet::net::PostResult signalResult =
            gamenet::net::PostResult::OwnerUnavailable;

        std::thread notifier([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            signalResult = source.signal();
        });
        std::thread quitter([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            loop->quit();
        });

        start.store(true, std::memory_order_release);
        notifier.join();
        quitter.join();
        operationsDonePromise.set_value();
        owner.join();

        GAMENET_TEST_ASSERT(signalResult != gamenet::net::PostResult::QueueFull);
        if (signalResult == gamenet::net::PostResult::Accepted) {
            GAMENET_TEST_ASSERT(executed.load() == 1);
        } else {
            GAMENET_TEST_ASSERT(
                signalResult == gamenet::net::PostResult::Shutdown ||
                signalResult == gamenet::net::PostResult::OwnerUnavailable);
            GAMENET_TEST_ASSERT(executed.load() == 0);
        }
        GAMENET_TEST_ASSERT(
            source.signal() == gamenet::net::PostResult::OwnerUnavailable);
    }
}

}  // namespace

int main() {
    testCommittedNotifySurvivesDetachAndSaturation();
    testLifecycleCapacityGenerationAndBudgetedSelfSignal();
    testLifecycleNotifyQuitLinearization();
    return 0;
}
