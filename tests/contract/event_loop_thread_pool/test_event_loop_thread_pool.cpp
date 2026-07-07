#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThreadPool.h"

#include "support/TestAssert.h"
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

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
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "worker-soak");
        pool.setThreadNum(3);
        pool.start();

        const auto workerLoops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(workerLoops.size() == 3);

        constexpr int submitterCount = 4;
        constexpr int iterationsPerWorker = 16;
        constexpr int expectedPerWorker = submitterCount * iterationsPerWorker;
        constexpr int expectedExecutions = expectedPerWorker * 3;

        const auto baseThread = std::this_thread::get_id();
        std::array<std::atomic<int>, 3> workerExecutions{};
        std::atomic<int> baseExecutions{0};
        std::atomic<int> totalExecutions{0};
        std::atomic<bool> observedAll{false};
        std::promise<void> allExecuted;
        auto allExecutedFuture = allExecuted.get_future();

        // event-loop-thread-pool-soak-contract: cross-thread queued work must
        // execute on the published worker loops, not on the base loop.
        std::vector<std::thread> submitters;
        for (int submitter = 0; submitter < submitterCount; ++submitter) {
            submitters.emplace_back([&, workerLoops] {
                for (int iteration = 0; iteration < iterationsPerWorker; ++iteration) {
                    for (std::size_t index = 0; index < workerLoops.size(); ++index) {
                        auto* loop = workerLoops[index];
                        loop->queueInLoop([&, loop, index] {
                            if (std::this_thread::get_id() == baseThread) {
                                ++baseExecutions;
                            }
                            GAMENET_TEST_ASSERT(loop->isInLoopThread());
                            ++workerExecutions[index];
                            const int total = totalExecutions.fetch_add(1) + 1;
                            if (total == expectedExecutions && !observedAll.exchange(true)) {
                                allExecuted.set_value();
                            }
                        });
                    }
                }
            });
        }

        for (auto& submitter : submitters) {
            submitter.join();
        }

        const auto status = allExecutedFuture.wait_for(std::chrono::seconds(2));
        GAMENET_TEST_ASSERT(status == std::future_status::ready);
        GAMENET_TEST_ASSERT(baseExecutions.load() == 0);
        GAMENET_TEST_ASSERT(totalExecutions.load() == expectedExecutions);
        for (const auto& executions : workerExecutions) {
            GAMENET_TEST_ASSERT(executions.load() == expectedPerWorker);
        }

        pool.stop();
        const auto stoppedLoops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(stoppedLoops.size() == 1);
        GAMENET_TEST_ASSERT(stoppedLoops.front() == &baseLoop);
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
