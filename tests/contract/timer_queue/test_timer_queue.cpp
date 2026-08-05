#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
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
