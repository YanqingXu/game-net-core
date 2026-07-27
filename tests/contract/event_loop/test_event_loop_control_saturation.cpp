#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopMetrics.h"

#include "support/TestAssert.h"

#include "../../../src/core/net/detail/EventLoopControlRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

using ControlRegistry = gamenet::net::detail::EventLoopControlRegistry;

void testBoundedCoalescedControlSources() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 2,
    });

    int throwingCalls = 0;
    int rearmingCalls = 0;
    int callbackDepth = 0;
    int callbackDepthPeak = 0;
    int normalCalls = 0;
    int controlExceptions = 0;
    std::size_t controlPendingPeak = 0;
    std::uint64_t firstControlWakeupCount = 0;

    loop.setCallbackExceptionHandler(
        [&](const gamenet::net::EventLoopCallbackException& failure) {
            GAMENET_TEST_ASSERT(
                failure.source == gamenet::net::EventLoopCallbackSource::Control);
            GAMENET_TEST_ASSERT(failure.exception != nullptr);
            ++controlExceptions;
            return gamenet::net::EventLoopCallbackExceptionAction::Continue;
        });
    loop.setEventLoopMetricCallback(
        [&](const gamenet::net::EventLoopMetricSample& sample) {
            if (sample.event ==
                gamenet::net::EventLoopMetricEvent::ControlSourcesDrained) {
                controlPendingPeak =
                    (std::max)(controlPendingPeak, sample.pendingControlSourcePeak);
                if (firstControlWakeupCount == 0) {
                    firstControlWakeupCount = sample.wakeupCount;
                }
            }
        });

    // control-callback-exception-isolated: the first slot throws, but the
    // later slot from the same fixed control round must still execute.
    auto throwingSource = ControlRegistry::registerSource(loop, [&] {
        ++throwingCalls;
        throw std::runtime_error("control source failure");
    });

    gamenet::net::EventLoopControlSource rearmingSource;
    rearmingSource = ControlRegistry::registerSource(loop, [&] {
        ++callbackDepth;
        callbackDepthPeak = (std::max)(callbackDepthPeak, callbackDepth);
        ++rearmingCalls;

        // control-self-notify-non-recursive: this sets the next-round mailbox
        // bit and must not recursively invoke the callback.
        if (rearmingCalls == 1) {
            GAMENET_TEST_ASSERT(
                rearmingSource.notify() == gamenet::net::PostResult::Accepted);
        } else if (rearmingCalls == 2) {
            // control-draining-self-rearm-only: after quit seals external
            // admission, this active callback retains only the capability to
            // re-arm its own source. Another owner-thread source is rejected.
            loop.quit();
            GAMENET_TEST_ASSERT(
                throwingSource.notify() == gamenet::net::PostResult::Shutdown);
            GAMENET_TEST_ASSERT(
                rearmingSource.notify() == gamenet::net::PostResult::Accepted);
        }
        --callbackDepth;
    });

    bool capacityFailureWasExplicit = false;
    try {
        (void)ControlRegistry::registerSource(loop, [] {});
    } catch (const std::length_error&) {
        capacityFailureWasExplicit = true;
    }
    GAMENET_TEST_ASSERT(capacityFailureWasExplicit);

    // Fill both normal and legacy reserved capacity before notifying either
    // control source.
    GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] { ++normalCalls; }));
    loop.queueInLoop([&] { ++normalCalls; });
    GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 2);

    // control-normal-reserve-saturation,
    // control-same-source-10000-coalesced, and
    // control-pending-does-not-rewake: no notify allocates a queue node,
    // consumes pending-functor capacity, or wakes again for an already-set bit.
    for (int index = 0; index < 10'000; ++index) {
        GAMENET_TEST_ASSERT(
            rearmingSource.notify() == gamenet::net::PostResult::Accepted);
    }
    GAMENET_TEST_ASSERT(
        throwingSource.notify() == gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(loop.pendingControlSourceCount() == 2);
    GAMENET_TEST_ASSERT(loop.mergedControlNotificationCount() == 9'999);

    // Avoid a long initial poll without changing the saturated functor queue.
    loop.runAfter(0ms, [] {});
    loop.loop();

    GAMENET_TEST_ASSERT(throwingCalls == 1);
    GAMENET_TEST_ASSERT(rearmingCalls == 3);
    GAMENET_TEST_ASSERT(callbackDepthPeak == 1);
    GAMENET_TEST_ASSERT(normalCalls == 2);
    GAMENET_TEST_ASSERT(controlExceptions == 1);
    GAMENET_TEST_ASSERT(loop.callbackExceptionCount() == 1);
    GAMENET_TEST_ASSERT(loop.pendingControlSourceCount() == 0);
    GAMENET_TEST_ASSERT(controlPendingPeak == 2);
    GAMENET_TEST_ASSERT(firstControlWakeupCount == 3);
    GAMENET_TEST_ASSERT(
        rearmingSource.notify() == gamenet::net::PostResult::Shutdown);
    GAMENET_TEST_ASSERT(
        loop.rejectedControlNotificationCount() == 2);

    ControlRegistry::unregisterSource(loop, rearmingSource);
    GAMENET_TEST_ASSERT(
        rearmingSource.notify() == gamenet::net::PostResult::OwnerUnavailable);
    ControlRegistry::unregisterSource(loop, throwingSource);
}

void testControlSourceCapacityValidation() {
    bool oversizedCapacityRejected = false;
    try {
        gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
            .maxControlSources = 65'537,
        });
    } catch (const std::invalid_argument&) {
        oversizedCapacityRejected = true;
    }
    GAMENET_TEST_ASSERT(oversizedCapacityRejected);
}

void testRepeatedQuitCannotResurrectExecutorOwnerIdentity() {
    gamenet::net::EventLoop loop;
    const auto executor = loop.executor();

    loop.runAfter(0ms, [&] { loop.quit(); });
    loop.loop();

    GAMENET_TEST_ASSERT(!executor.available());
    GAMENET_TEST_ASSERT(!executor.isInOwnerThread());
    GAMENET_TEST_ASSERT(
        executor.post([] {}) == gamenet::net::PostResult::Shutdown);

    loop.quit();
    loop.quit();

    GAMENET_TEST_ASSERT(!executor.available());
    GAMENET_TEST_ASSERT(!executor.isInOwnerThread());
    GAMENET_TEST_ASSERT(
        executor.post([] {}) == gamenet::net::PostResult::Shutdown);
}

void testNotifyQuitLinearization() {
    // control-notify-quit-linearization: every Accepted result must execute in
    // the final control drain. A request that loses the quit race must return
    // Shutdown or OwnerUnavailable, never QueueFull or a partial acceptance.
    constexpr int repetitions = 100;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        std::promise<gamenet::net::EventLoop*> loopPromise;
        auto loopFuture = loopPromise.get_future();
        std::promise<gamenet::net::EventLoopControlSource> sourcePromise;
        auto sourceFuture = sourcePromise.get_future();
        std::promise<void> operationsDonePromise;
        auto operationsDoneFuture = operationsDonePromise.get_future();
        std::atomic<int> executed{0};

        std::thread owner([&] {
            gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
                .maxPendingFunctors = 1,
                .reservedPendingFunctors = 0,
                .maxFunctorsPerIteration = 1,
                .maxControlSources = 1,
            });
            auto source =
                ControlRegistry::registerSource(loop, [&] { ++executed; });
            loopPromise.set_value(&loop);
            sourcePromise.set_value(source);
            loop.loop();
            // Keep the raw EventLoop pointer valid until both racing
            // operations have returned and no caller can still dereference it.
            operationsDoneFuture.wait();
            ControlRegistry::unregisterSource(loop, source);
        });

        gamenet::net::EventLoop* loop = loopFuture.get();
        auto source = sourceFuture.get();
        std::atomic<bool> start{false};
        gamenet::net::PostResult notifyResult =
            gamenet::net::PostResult::OwnerUnavailable;

        std::thread notifier([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            notifyResult = source.notify();
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

        GAMENET_TEST_ASSERT(notifyResult != gamenet::net::PostResult::QueueFull);
        if (notifyResult == gamenet::net::PostResult::Accepted) {
            GAMENET_TEST_ASSERT(executed.load() == 1);
        } else {
            GAMENET_TEST_ASSERT(
                notifyResult == gamenet::net::PostResult::Shutdown ||
                notifyResult == gamenet::net::PostResult::OwnerUnavailable);
            GAMENET_TEST_ASSERT(executed.load() == 0);
        }
        GAMENET_TEST_ASSERT(
            source.notify() == gamenet::net::PostResult::OwnerUnavailable);
    }
}

}  // namespace

int main() {
    testBoundedCoalescedControlSources();
    testControlSourceCapacityValidation();
    testRepeatedQuitCannotResurrectExecutorOwnerIdentity();
    testNotifyQuitLinearization();
    return 0;
}
