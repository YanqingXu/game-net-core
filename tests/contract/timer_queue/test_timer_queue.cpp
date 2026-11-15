// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "../../../src/core/net/detail/EventLoopLifecycleRegistry.h"
#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

enum class PendingCancel { BeforeDrain, EarlierFunctor, QuitRetirement };

static void cancelBeforeInsertion(PendingCancel order, bool repeating) {
    using namespace gamenet::net;
    EventLoop loop({.maxPendingFunctors = 4, .reservedPendingFunctors = 0,
                    .maxFunctorsPerIteration = 4});
    const auto owner = std::this_thread::get_id();
    unsigned destroyed = 0, fired = 0;
    PostResult reentrant = PostResult::QueueFull;
    auto other = loop.runAfter(1h, [] { GAMENET_TEST_FAIL("other timer survived cancellation"); });
    struct Capture {
        EventLoop& loop;
        TimerId other;
        std::thread::id owner;
        unsigned& destroyed;
        PostResult& result;
        Capture(EventLoop& l, TimerId id, std::thread::id o, unsigned& d, PostResult& r)
            : loop(l), other(id), owner(o), destroyed(d), result(r) {}
        ~Capture() {
            GAMENET_TEST_ASSERT(std::this_thread::get_id() == owner);
            ++destroyed;
            result = loop.tryCancel(other);
        }
    };
    auto capture = std::make_shared<Capture>(loop, other, owner, destroyed, reentrant);
    std::weak_ptr<Capture> weak = capture;
    TimerScheduleResult scheduled;
    auto cancel = [&] {
        if (order == PendingCancel::QuitRetirement) {
            loop.quit();
            GAMENET_TEST_ASSERT(loop.tryCancel(scheduled.timerId) == PostResult::Shutdown);
            detail::EventLoopLifecycleRegistry::retireTimer(loop, scheduled.timerId);
            GAMENET_TEST_ASSERT(!scheduled.timerId.valid());
        } else {
            GAMENET_TEST_ASSERT(loop.tryCancel(scheduled.timerId) == PostResult::Accepted);
            GAMENET_TEST_ASSERT(loop.tryCancel(scheduled.timerId) == PostResult::Accepted);
        }
        // Captures retire on owner even while the add closure still owns metadata.
        GAMENET_TEST_ASSERT(weak.expired() && destroyed == 1);
    };
    if (order == PendingCancel::EarlierFunctor)
        GAMENET_TEST_ASSERT(loop.executor().post(cancel) == PostResult::Accepted);
    std::thread scheduler([&] {
        auto callback = [capture, &fired] { ++fired; };
        scheduled = repeating ? loop.tryRunEvery(1h, std::move(callback))
                              : loop.tryRunAfter(order == PendingCancel::QuitRetirement ? 1h : 0ms,
                                                 std::move(callback));
    });
    scheduler.join();
    capture.reset();
    GAMENET_TEST_ASSERT(scheduled.result == PostResult::Accepted && scheduled.timerId.valid());
    if (order != PendingCancel::EarlierFunctor)
        cancel();
    if (order == PendingCancel::QuitRetirement) {
        GAMENET_TEST_ASSERT(reentrant == PostResult::Shutdown);
        detail::EventLoopLifecycleRegistry::retireTimer(loop, other);
        loop.loop(); // The accepted add must not recreate the retired registration.
    } else {
        // EarlierFunctor cancels an add already extracted into this same batch.
        detail::EventLoopActiveBatchHarness::runPendingFunctors(loop, 4);
        GAMENET_TEST_ASSERT(reentrant == PostResult::Accepted);
    }
    std::size_t drained = 0;
    loop.setEventLoopMetricCallback([&](const EventLoopMetricSample& sample) {
        if (sample.event == EventLoopMetricEvent::TimersDrained)
            drained += sample.drainedWork;
    });
    detail::EventLoopActiveBatchHarness::runExpiredTimers(loop, gamenet::base::now() + 2h);
    GAMENET_TEST_ASSERT(!fired && !drained && !loop.callbackExceptionCount() &&
                        weak.expired() && destroyed == 1 && !loop.pendingFunctorCount());
    loop.setEventLoopMetricCallback({});
    if (order != PendingCancel::QuitRetirement) {
        loop.quit();
        loop.loop();
    }
}

static void boundedPendingAdmission() {
    using namespace gamenet::net;
    EventLoop loop({.maxPendingFunctors = 1, .reservedPendingFunctors = 0,
                    .maxFunctorsPerIteration = 1});
    auto schedule = [&](PostResult expected) {
        auto capture = std::make_shared<int>(42);
        std::weak_ptr<int> weak = capture;
        TimerScheduleResult result;
        std::thread foreign([&] { result = loop.tryRunAfter(0ms, [capture] {
            GAMENET_TEST_FAIL("cancelled/rejected pending timer fired");
        }); });
        foreign.join();
        capture.reset();
        GAMENET_TEST_ASSERT(result.result == expected);
        if (expected == PostResult::Accepted) {
            GAMENET_TEST_ASSERT(result.timerId.valid() && !weak.expired());
            GAMENET_TEST_ASSERT(loop.tryCancel(result.timerId) == PostResult::Accepted);
        } else
            GAMENET_TEST_ASSERT(!result.timerId.valid());
        GAMENET_TEST_ASSERT(weak.expired());
        return result.timerId;
    };
    GAMENET_TEST_ASSERT(loop.executor().post([] {}) == PostResult::Accepted);
    (void)schedule(PostResult::QueueFull); // Post rejection rolls back weak registration.
    detail::EventLoopActiveBatchHarness::runPendingFunctors(loop, 1);
    for (unsigned round = 0; round < 16; ++round) {
        const auto id = schedule(PostResult::Accepted);
        (void)schedule(PostResult::QueueFull); // Cancelled metadata still occupies its add slot.
        detail::EventLoopActiveBatchHarness::runPendingFunctors(loop, 1);
        for (unsigned stale = 0; stale < 16; ++stale)
            GAMENET_TEST_ASSERT(loop.tryCancel(id) == PostResult::Accepted);
        // The next round must recover capacity despite all stale cancellations.
    }
    std::size_t drained = 0;
    loop.setEventLoopMetricCallback([&](const EventLoopMetricSample& sample) {
        if (sample.event == EventLoopMetricEvent::TimersDrained)
            drained += sample.drainedWork;
    });
    detail::EventLoopActiveBatchHarness::runExpiredTimers(loop, gamenet::base::now() + 1h);
    GAMENET_TEST_ASSERT(!drained && !loop.callbackExceptionCount());
    loop.setEventLoopMetricCallback({});
    loop.quit();
    loop.loop();
    std::thread afterQuit([&] {
        GAMENET_TEST_ASSERT(loop.tryRunAfter(1h, [] {}).result == PostResult::Shutdown);
    });
    afterQuit.join();
}

static void pendingAdmissionTeardown() {
    for (unsigned round = 0; round < 16; ++round) {
        std::weak_ptr<int> weak;
        {
            gamenet::net::EventLoop loop({.maxPendingFunctors = 1, .reservedPendingFunctors = 0,
                                         .maxFunctorsPerIteration = 1});
            auto capture = std::make_shared<int>(42);
            weak = capture;
            std::thread foreign([&] {
                GAMENET_TEST_ASSERT(loop.tryRunAfter(1h, [capture] {}).result ==
                                    gamenet::net::PostResult::Accepted);
            });
            foreign.join();
            capture.reset();
            GAMENET_TEST_ASSERT(!weak.expired());
        } // Undrained teardown must remove pending observations, too.
        GAMENET_TEST_ASSERT(weak.expired());
    }
}

static void extractedPendingAdmissionBound(std::barrier<>* ready = nullptr) {
    using namespace gamenet::net;
    EventLoop loop({.maxPendingFunctors = 2, .reservedPendingFunctors = 0,
                    .maxFunctorsPerIteration = 2});
    if (ready) ready->arrive_and_wait();
    std::array<TimerId, 5> ids;
    std::array<std::weak_ptr<int>, 5> captures;
    auto schedule = [&](std::size_t index, PostResult expected) {
        auto capture = std::make_shared<int>(42);
        captures[index] = capture;
        TimerScheduleResult result;
        std::thread foreign([&] {
            result = loop.tryRunAfter(1h, [capture] {
                GAMENET_TEST_FAIL("bounded pending timer escaped retirement");
            });
        });
        foreign.join();
        capture.reset();
        GAMENET_TEST_ASSERT(result.result == expected);
        GAMENET_TEST_ASSERT(result.timerId.valid() == (expected == PostResult::Accepted));
        GAMENET_TEST_ASSERT(captures[index].expired() == (expected != PostResult::Accepted));
        ids[index] = result.timerId;
    };
    GAMENET_TEST_ASSERT(loop.executor().post([&] {
        // Timer 0 has left the queue but has not run its extracted add functor.
        schedule(1, PostResult::Accepted);
        GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 1); // One physical queue slot is free.
        schedule(2, PostResult::QueueFull); // The two pending metadata slots are both occupied.
        for (auto index : {0, 1}) {
            GAMENET_TEST_ASSERT(loop.tryCancel(ids[index]) == PostResult::Accepted);
            GAMENET_TEST_ASSERT(captures[index].expired());
        }
    }) == PostResult::Accepted);
    schedule(0, PostResult::Accepted);
    detail::EventLoopActiveBatchHarness::runPendingFunctors(loop, 2);
    // Timer 0's insertion has drained; timer 1's cancelled metadata remains queued.
    schedule(3, PostResult::Accepted);
    loop.quit();
    detail::EventLoopLifecycleRegistry::retireTimer(loop, ids[3]);
    GAMENET_TEST_ASSERT(!ids[3].valid() && captures[3].expired());
    schedule(4, PostResult::Shutdown); // Shutdown wins even when the pending index is full.
    loop.loop();
    std::size_t drained = 0;
    loop.setEventLoopMetricCallback([&](const EventLoopMetricSample& sample) {
        if (sample.event == EventLoopMetricEvent::TimersDrained)
            drained += sample.drainedWork;
    });
    detail::EventLoopActiveBatchHarness::runExpiredTimers(loop, gamenet::base::now() + 2h);
    GAMENET_TEST_ASSERT(!drained && !loop.callbackExceptionCount() && !loop.pendingFunctorCount());
    loop.setEventLoopMetricCallback({});
}

static void lifecycleTimerRetirement(bool afterDrain) {
    using namespace gamenet::net;
    EventLoop loop({.maxPendingFunctors = 1, .reservedPendingFunctors = 0,
                    .maxFunctorsPerIteration = 1});
    auto otherCapture = std::make_shared<int>(42);
    std::weak_ptr<int> otherWeak = otherCapture;
    auto other = loop.runAfter(1h, [otherCapture] { GAMENET_TEST_FAIL("other timer fired"); });
    otherCapture.reset();
    PostResult reentrant = PostResult::Accepted;
    struct Capture {
        EventLoop& loop;
        TimerId other;
        PostResult& result;
        Capture(EventLoop& l, TimerId id, PostResult& r) : loop(l), other(id), result(r) {}
        ~Capture() { result = loop.tryCancel(other); }
    };
    auto capture = std::make_shared<Capture>(loop, other, reentrant);
    std::weak_ptr<Capture> weak = capture;
    auto timer = loop.runEvery(1h, [capture] { GAMENET_TEST_FAIL("retired timer fired"); });
    capture.reset();
    GAMENET_TEST_ASSERT(loop.executor().post([] {}) == PostResult::Accepted);
    std::thread quitter([&] { loop.quit(); });
    quitter.join();
    if (afterDrain)
        loop.loop();
    GAMENET_TEST_ASSERT(loop.tryCancel(timer) == PostResult::Shutdown && !weak.expired());
    detail::EventLoopLifecycleRegistry::retireTimer(loop, timer);
    GAMENET_TEST_ASSERT(!timer.valid() && weak.expired() &&
                        reentrant == PostResult::Shutdown && !otherWeak.expired());
    detail::EventLoopLifecycleRegistry::retireTimer(loop, other);
    GAMENET_TEST_ASSERT(!other.valid() && otherWeak.expired());
    detail::EventLoopLifecycleRegistry::retireTimer(loop, timer); // Already retired is harmless.
    if (!afterDrain)
        loop.loop();
}

static void retireExtractedTimerAfterQuit() {
    using namespace gamenet::net;
    EventLoop loop;
    TimerId later;
    unsigned fired = 0;
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak = capture;
    const auto when = gamenet::base::now();
    loop.runAt(when, [&] {
        loop.quit();
        GAMENET_TEST_ASSERT(loop.tryCancel(later) == PostResult::Shutdown);
        // Both timers have the same ready deadline and fit the default batch.
        detail::EventLoopLifecycleRegistry::retireTimer(loop, later);
        GAMENET_TEST_ASSERT(!later.valid());
    });
    later = loop.runAt(when, [capture, &fired] { ++fired; });
    capture.reset();
    loop.loop();
    GAMENET_TEST_ASSERT(!fired && weak.expired());
}

int main() {
    for (auto order : {PendingCancel::BeforeDrain, PendingCancel::EarlierFunctor,
                       PendingCancel::QuitRetirement})
        for (bool repeating : {false, true})
            cancelBeforeInsertion(order, repeating);
    boundedPendingAdmission();
    pendingAdmissionTeardown();
    extractedPendingAdmissionBound();
    std::barrier ready(4);
    std::vector<std::thread> owners;
    for (unsigned n = 0; n < 4; ++n)
        owners.emplace_back([&] { extractedPendingAdmissionBound(&ready); });
    for (auto& owner : owners) owner.join();
    lifecycleTimerRetirement(false);
    lifecycleTimerRetirement(true);
    retireExtractedTimerAfterQuit();
    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();
        const auto callerThread = std::this_thread::get_id();

        std::promise<std::thread::id> firedOn;
        auto firedFuture = firedOn.get_future();

        loop->runAfter(20ms, [loop, &firedOn] {
            firedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        gamenet::test::waitUntilReady(firedFuture, 1s, "one-shot timer did not fire");
        GAMENET_TEST_ASSERT(firedFuture.get() != callerThread);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> fired{false};
        auto timerId = loop->runAfter(50ms, [&] { fired = true; });
        std::promise<void> finished;
        auto finishedFuture = finished.get_future();

        loop->runAfter(80ms, [loop, &finished] {
            finished.set_value();
            loop->quit();
        });

        GAMENET_TEST_ASSERT(!loop->isInLoopThread());
        loop->cancel(timerId);

        gamenet::test::waitUntilReady(finishedFuture, 1s, "cancel-before-expiration observer did not finish");
        GAMENET_TEST_ASSERT(!fired.load());
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> firstFired{false};
        std::atomic<bool> secondFired{false};
        std::atomic<bool> observed{false};
        std::promise<void> finished;
        auto finishedFuture = finished.get_future();
        auto second = std::make_shared<gamenet::net::TimerId>();

        loop->runInLoop([loop, second, &firstFired, &secondFired, &observed, &finished] {
            const auto when = gamenet::base::now() + 20ms;

            // timer-cancel-ready-contract: canceling a timer from an earlier
            // ready callback prevents the later callback from firing.
            loop->runAt(when, [loop, second, &firstFired, &observed, &finished] {
                firstFired = true;
                loop->cancel(*second);
                loop->runAfter(40ms, [loop, &observed, &finished] {
                    if (!observed.exchange(true)) {
                        finished.set_value();
                    }
                    loop->quit();
                });
            });

            *second = loop->runAt(when, [&secondFired] {
                secondFired = true;
            });

            loop->runAfter(250ms, [loop, &observed, &finished] {
                if (!observed.exchange(true)) {
                    finished.set_value();
                }
                loop->quit();
            });
        });

        gamenet::test::waitUntilReady(finishedFuture, 1s, "ready-timer cancellation observer did not finish");
        GAMENET_TEST_ASSERT(firstFired.load());
        GAMENET_TEST_ASSERT(!secondFired.load());
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<int> firedCount;
        auto firedCountFuture = firedCount.get_future();
        auto repeating = std::make_shared<gamenet::net::TimerId>();

        loop->runInLoop([loop, repeating, &firedCount] {
            auto count = std::make_shared<int>(0);
            *repeating = loop->runEvery(10ms, [loop, repeating, count, &firedCount] {
                ++*count;
                if (*count == 3) {
                    loop->cancel(*repeating);
                    loop->runAfter(30ms, [loop, count, &firedCount] {
                        firedCount.set_value(*count);
                        loop->quit();
                    });
                }
            });
        });

        gamenet::test::waitUntilReady(firedCountFuture, 1s, "repeating timer did not reach cancellation count");
        GAMENET_TEST_ASSERT(firedCountFuture.get() == 3);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::vector<std::string>> finished;
        auto finishedFuture = finished.get_future();
        auto repeating = std::make_shared<gamenet::net::TimerId>();
        auto order = std::make_shared<std::vector<std::string>>();
        auto count = std::make_shared<int>(0);

        loop->runInLoop([loop, repeating, order, count, &finished] {
            // timer-fixed-delay-contract: a late legacy callback schedules the
            // next occurrence from completion, so already-ready work runs first.
            *repeating = loop->runEvery(10ms, [loop, repeating, order, count, &finished] {
                ++*count;
                order->push_back("repeat-" + std::to_string(*count));
                if (*count == 1) {
                    loop->runAfter(0ms, [order] { order->push_back("sentinel"); });
                    std::this_thread::sleep_for(60ms);
                    return;
                }

                loop->cancel(*repeating);
                finished.set_value(*order);
                loop->quit();
            });
        });

        gamenet::test::waitUntilReady(
            finishedFuture,
            1s,
            "legacy fixed-delay ordering did not converge");
        GAMENET_TEST_ASSERT(
            finishedFuture.get() ==
            std::vector<std::string>({"repeat-1", "sentinel", "repeat-2"}));
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::vector<std::string>> finished;
        auto finishedFuture = finished.get_future();
        auto repeating = std::make_shared<gamenet::net::TimerId>();
        auto order = std::make_shared<std::vector<std::string>>();
        auto count = std::make_shared<int>(0);

        loop->runInLoop([loop, repeating, order, count, &finished] {
            const gamenet::net::RepeatingTimerOptions options{
                .mode = gamenet::net::RepeatingTimerMode::FixedRate,
                .maxCatchUpCallbacks = 2,
            };
            // timer-fixed-rate-catch-up-contract: two missed cadence points may
            // replay; after that the timer skips ahead and yields to sentinel-2.
            *repeating = loop->runEvery(
                10ms,
                [loop, repeating, order, count, &finished] {
                    ++*count;
                    order->push_back("repeat-" + std::to_string(*count));
                    if (*count == 1) {
                        std::this_thread::sleep_for(80ms);
                        loop->runAfter(0ms, [loop, repeating, order, &finished] {
                            order->push_back("sentinel-1");
                            loop->runAfter(0ms, [loop, repeating, order, &finished] {
                                order->push_back("sentinel-2");
                                loop->cancel(*repeating);
                                finished.set_value(*order);
                                loop->quit();
                            });
                        });
                    }
                },
                options);
        });

        gamenet::test::waitUntilReady(
            finishedFuture,
            1s,
            "fixed-rate bounded catch-up ordering did not converge");
        GAMENET_TEST_ASSERT(
            finishedFuture.get() ==
            std::vector<std::string>({
                "repeat-1",
                "repeat-2",
                "sentinel-1",
                "repeat-3",
                "sentinel-2",
            }));
    }

    {
        gamenet::net::EventLoop loop;
        bool rejected = false;
        try {
            (void)loop.runEvery(
                10ms,
                [] {},
                gamenet::net::RepeatingTimerOptions{
                    .mode = gamenet::net::RepeatingTimerMode::FixedDelay,
                    .maxCatchUpCallbacks = 1,
                });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);

        rejected = false;
        try {
            (void)loop.runEvery(
                10ms,
                [] {},
                gamenet::net::RepeatingTimerOptions{
                    .mode = static_cast<gamenet::net::RepeatingTimerMode>(99),
                });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);
    }

    {
        gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
            .maxPendingFunctors = 1,
            .reservedPendingFunctors = 0,
            .maxFunctorsPerIteration = 1,
        });
        const auto retained = loop.tryRunAfter(1s, [] {});
        GAMENET_TEST_ASSERT(
            retained.result == gamenet::net::PostResult::Accepted);
        GAMENET_TEST_ASSERT(retained.timerId.valid());

        GAMENET_TEST_ASSERT(loop.tryQueueInLoop([] {}));
        gamenet::net::TimerScheduleResult saturated;
        std::thread scheduler([&] {
            saturated = loop.tryRunAfter(1s, [] {});
        });
        scheduler.join();
        GAMENET_TEST_ASSERT(
            saturated.result == gamenet::net::PostResult::QueueFull);
        GAMENET_TEST_ASSERT(!saturated.timerId.valid());

        loop.quit();
        const auto stopped = loop.tryRunAfter(1s, [] {});
        GAMENET_TEST_ASSERT(
            stopped.result == gamenet::net::PostResult::Shutdown);
        GAMENET_TEST_ASSERT(!stopped.timerId.valid());
        GAMENET_TEST_ASSERT(
            loop.tryCancel(retained.timerId) ==
            gamenet::net::PostResult::Shutdown);

        bool legacyShutdownWasExplicit = false;
        try {
            (void)loop.runAfter(1s, [] {});
        } catch (const std::logic_error&) {
            legacyShutdownWasExplicit = true;
        }
        GAMENET_TEST_ASSERT(legacyShutdownWasExplicit);
    }

    {
        gamenet::net::EventLoop loop;
        bool rejected = false;
        try {
            (void)loop.tryRunEvery(-1ms, [] {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);

        rejected = false;
        try {
            (void)loop.tryRunEvery(0ms, [] {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();
        const auto callerThread = std::this_thread::get_id();

        std::promise<std::thread::id> firedOn;
        auto firedFuture = firedOn.get_future();
        const auto when = gamenet::base::now() + 20ms;

        std::thread scheduler([loop, when, &firedOn] {
            loop->runAt(when, [loop, &firedOn] {
                firedOn.set_value(std::this_thread::get_id());
                loop->quit();
            });
        });

        gamenet::test::waitUntilReady(firedFuture, 1s, "cross-thread scheduled timer did not fire");
        GAMENET_TEST_ASSERT(firedFuture.get() != callerThread);
        scheduler.join();
    }

    return 0;
}
