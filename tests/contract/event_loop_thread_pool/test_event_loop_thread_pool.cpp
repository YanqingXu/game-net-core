#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThreadPool.h"

#include "support/TestAssert.h"
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

int main() {
    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "zero");
        bool callbackRan = false;
        pool.start([&](gamenet::net::EventLoop* loop) {
            callbackRan = true;
            GAMENET_TEST_ASSERT(loop == &baseLoop);
            GAMENET_TEST_ASSERT(loop->isInLoopThread());
        });

        GAMENET_TEST_ASSERT(pool.getNextLoop() == &baseLoop);
        const auto loops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(loops.size() == 1);
        GAMENET_TEST_ASSERT(loops.front() == &baseLoop);
        GAMENET_TEST_ASSERT(callbackRan);
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "workers");
        pool.setThreadNum(2);
        pool.start();

        const auto loops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(loops.size() == 2);
        GAMENET_TEST_ASSERT(loops[0] != nullptr);
        GAMENET_TEST_ASSERT(loops[1] != nullptr);
        GAMENET_TEST_ASSERT(loops[0] != loops[1]);

        GAMENET_TEST_ASSERT(pool.getNextLoop() == loops[0]);
        GAMENET_TEST_ASSERT(pool.getNextLoop() == loops[1]);
        GAMENET_TEST_ASSERT(pool.getNextLoop() == loops[0]);

        pool.stop();
        const auto stoppedLoops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(stoppedLoops.size() == 1);
        GAMENET_TEST_ASSERT(stoppedLoops.front() == &baseLoop);
        GAMENET_TEST_ASSERT(pool.getNextLoop() == &baseLoop);
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "wrong-thread-start");

        std::promise<void> observed;
        auto observedFuture = observed.get_future();
        std::thread worker([&] {
            try {
                pool.start();
                GAMENET_TEST_ASSERT(false);
            } catch (const std::runtime_error&) {
                observed.set_value();
            }
        });

        const auto status = observedFuture.wait_for(std::chrono::seconds(1));
        GAMENET_TEST_ASSERT(status == std::future_status::ready);
        worker.join();
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "wrong-thread-next");
        pool.setThreadNum(1);
        pool.start();

        std::promise<void> observed;
        auto observedFuture = observed.get_future();
        std::thread worker([&] {
            try {
                (void)pool.getNextLoop();
                GAMENET_TEST_ASSERT(false);
            } catch (const std::runtime_error&) {
                observed.set_value();
            }
        });

        const auto status = observedFuture.wait_for(std::chrono::seconds(1));
        GAMENET_TEST_ASSERT(status == std::future_status::ready);
        worker.join();
        pool.stop();
    }

    return 0;
}

