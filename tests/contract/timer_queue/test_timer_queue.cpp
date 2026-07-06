#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <future>
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

        GAMENET_TEST_ASSERT(firedFuture.wait_for(1s) == std::future_status::ready);
        GAMENET_TEST_ASSERT(firedFuture.get() != callerThread);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> fired{false};
        auto timerId = loop->runAfter(50ms, [&] { fired = true; });
        loop->runAfter(80ms, [loop] { loop->quit(); });

        std::thread canceller([loop, timerId] {
            std::this_thread::sleep_for(10ms);
            loop->cancel(timerId);
        });

        std::this_thread::sleep_for(120ms);
        canceller.join();
        GAMENET_TEST_ASSERT(!fired.load());
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<int> firedCount;
        auto firedCountFuture = firedCount.get_future();
        int count = 0;
        gamenet::net::TimerId repeating;

        repeating = loop->runEvery(10ms, [loop, &count, &firedCount, &repeating] {
            ++count;
            if (count == 3) {
                loop->cancel(repeating);
                loop->runAfter(30ms, [loop, &firedCount, &count] {
                    firedCount.set_value(count);
                    loop->quit();
                });
            }
        });

        GAMENET_TEST_ASSERT(firedCountFuture.wait_for(1s) == std::future_status::ready);
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

        GAMENET_TEST_ASSERT(firedFuture.wait_for(1s) == std::future_status::ready);
        GAMENET_TEST_ASSERT(firedFuture.get() != callerThread);
        scheduler.join();
    }

    return 0;
}
