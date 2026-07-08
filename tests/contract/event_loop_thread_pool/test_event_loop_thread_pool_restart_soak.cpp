#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThreadPool.h"

#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <thread>

int main() {
    constexpr int iterationCount = 5;
    constexpr int workerCount = 2;

    gamenet::net::EventLoop baseLoop;
    gamenet::net::EventLoopThreadPool pool(&baseLoop, "restart-soak");
    pool.setThreadNum(workerCount);

    const auto baseThread = std::this_thread::get_id();

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        pool.start();

        const auto workerLoops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(workerLoops.size() == workerCount);

        std::atomic<int> executed{0};
        std::atomic<int> baseExecutions{0};
        std::mutex workerThreadIdsMutex;
        std::set<std::thread::id> workerThreadIds;
        std::promise<void> allExecuted;
        auto allExecutedFuture = allExecuted.get_future();

        // event-loop-thread-pool-restart-soak-contract: after each stop(),
        // restart must publish fresh worker loops that accept queued work and
        // stop must return the pool to base-loop fallback selection.
        for (auto* loop : workerLoops) {
            loop->queueInLoop([&, loop] {
                GAMENET_TEST_ASSERT(loop->isInLoopThread());
                if (std::this_thread::get_id() == baseThread) {
                    ++baseExecutions;
                }
                {
                    std::lock_guard lock(workerThreadIdsMutex);
                    workerThreadIds.insert(std::this_thread::get_id());
                }

                const int done = executed.fetch_add(1) + 1;
                if (done == workerCount) {
                    allExecuted.set_value();
                }
            });
        }

        const auto status = allExecutedFuture.wait_for(std::chrono::seconds(2));
        GAMENET_TEST_ASSERT(status == std::future_status::ready);
        GAMENET_TEST_ASSERT(executed.load() == workerCount);
        GAMENET_TEST_ASSERT(baseExecutions.load() == 0);
        {
            std::lock_guard lock(workerThreadIdsMutex);
            GAMENET_TEST_ASSERT(workerThreadIds.size() == workerCount);
        }

        pool.stop();
        const auto stoppedLoops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(stoppedLoops.size() == 1);
        GAMENET_TEST_ASSERT(stoppedLoops.front() == &baseLoop);
        GAMENET_TEST_ASSERT(pool.getNextLoop() == &baseLoop);
    }

    return 0;
}
