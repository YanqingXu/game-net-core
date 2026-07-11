#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

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
