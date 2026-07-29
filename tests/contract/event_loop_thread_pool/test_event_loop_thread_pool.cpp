#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThreadPool.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace gamenet::net::detail {

class EventLoopThreadPoolSelectionHarness {
public:
    static void connectionOpened(
        EventLoopThreadPool& pool,
        EventLoop* loop) {
        pool.recordConnectionOpened(loop);
    }

    static void connectionClosed(
        EventLoopThreadPool& pool,
        EventLoop* loop) {
        pool.recordConnectionClosed(loop);
    }

    static std::size_t connectionLoad(
        const EventLoopThreadPool& pool,
        EventLoop* loop) {
        return pool.connectionLoad(loop);
    }
};

}  // namespace gamenet::net::detail

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
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "least-connections");
        pool.setThreadNum(3);
        pool.setLoopSelectionPolicy(
            gamenet::net::EventLoopSelectionPolicy::LeastConnections);
        pool.start();

        const auto loops = pool.getAllLoops();
        using Harness =
            gamenet::net::detail::EventLoopThreadPoolSelectionHarness;

        // event-loop-thread-pool-least-connections-contract: base-loop-owned
        // load commits and releases determine selection; ties rotate.
        auto* first = pool.selectLoop();
        GAMENET_TEST_ASSERT(first == loops[0]);
        Harness::connectionOpened(pool, first);

        auto* second = pool.selectLoop();
        GAMENET_TEST_ASSERT(second == loops[1]);
        Harness::connectionOpened(pool, second);

        auto* third = pool.selectLoop();
        GAMENET_TEST_ASSERT(third == loops[2]);
        Harness::connectionOpened(pool, third);
        Harness::connectionOpened(pool, third);

        GAMENET_TEST_ASSERT(pool.selectLoop() == loops[0]);
        Harness::connectionClosed(pool, loops[0]);
        GAMENET_TEST_ASSERT(pool.selectLoop() == loops[0]);
        GAMENET_TEST_ASSERT(Harness::connectionLoad(pool, loops[0]) == 0);
        GAMENET_TEST_ASSERT(Harness::connectionLoad(pool, loops[1]) == 1);
        GAMENET_TEST_ASSERT(Harness::connectionLoad(pool, loops[2]) == 2);

        Harness::connectionClosed(pool, loops[1]);
        Harness::connectionClosed(pool, loops[2]);
        Harness::connectionClosed(pool, loops[2]);

        bool underflowRejected = false;
        try {
            Harness::connectionClosed(pool, loops[0]);
        } catch (const std::logic_error&) {
            underflowRejected = true;
        }
        GAMENET_TEST_ASSERT(underflowRejected);
        pool.stop();
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "queue-lag");
        pool.setThreadNum(2);
        pool.setLoopSelectionPolicy(
            gamenet::net::EventLoopSelectionPolicy::QueueLag);
        pool.start();

        const auto loops = pool.getAllLoops();
        // Empty-queue ties rotate instead of pinning worker zero.
        GAMENET_TEST_ASSERT(pool.selectLoop() == loops[0]);
        GAMENET_TEST_ASSERT(pool.selectLoop() == loops[1]);

        std::promise<void> blockerStarted;
        auto blockerStartedFuture = blockerStarted.get_future();
        std::promise<void> releaseBlocker;
        auto releaseBlockerFuture = releaseBlocker.get_future().share();
        std::promise<void> queuedWorkFinished;
        auto queuedWorkFinishedFuture = queuedWorkFinished.get_future();

        loops[0]->queueInLoop(
            [&blockerStarted, releaseBlockerFuture] {
                blockerStarted.set_value();
                releaseBlockerFuture.wait();
            });
        gamenet::test::waitUntilReady(
            blockerStartedFuture,
            std::chrono::seconds(1),
            "queue-lag blocker did not start");
        loops[0]->queueInLoop(
            [&queuedWorkFinished] { queuedWorkFinished.set_value(); });

        // event-loop-thread-pool-queue-lag-contract: the idle queue wins over
        // a worker with an older pending functor.
        GAMENET_TEST_ASSERT(pool.selectLoop() == loops[1]);
        releaseBlocker.set_value();
        gamenet::test::waitUntilReady(
            queuedWorkFinishedFuture,
            std::chrono::seconds(1),
            "queue-lag queued work did not finish");
        pool.stop();
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "consistent-hash");
        pool.setThreadNum(4);
        pool.setLoopSelectionPolicy(
            gamenet::net::EventLoopSelectionPolicy::ConsistentHash);
        pool.start();

        const auto loops = pool.getAllLoops();
        std::set<gamenet::net::EventLoop*> selectedLoops;
        for (int index = 0; index < 128; ++index) {
            const std::string key = "player-" + std::to_string(index);
            auto* selected = pool.selectLoop(key);
            // event-loop-thread-pool-consistent-hash-contract: a key is stable
            // for a fixed indexed worker set.
            GAMENET_TEST_ASSERT(pool.selectLoop(key) == selected);
            selectedLoops.insert(selected);
        }
        GAMENET_TEST_ASSERT(selectedLoops.size() == loops.size());

        bool emptyKeyRejected = false;
        try {
            (void)pool.selectLoop();
        } catch (const std::invalid_argument&) {
            emptyKeyRejected = true;
        }
        GAMENET_TEST_ASSERT(emptyKeyRejected);

        bool latePolicyRejected = false;
        try {
            pool.setLoopSelectionPolicy(
                gamenet::net::EventLoopSelectionPolicy::RoundRobin);
        } catch (const std::logic_error&) {
            latePolicyRejected = true;
        }
        GAMENET_TEST_ASSERT(latePolicyRejected);
        pool.stop();
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

        gamenet::test::waitUntilReady(
            observedFuture,
            std::chrono::seconds(1),
            "wrong-thread start did not report runtime_error");
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

        gamenet::test::waitUntilReady(
            allExecutedFuture,
            std::chrono::seconds(2),
            "worker queued work did not finish");
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

        gamenet::test::waitUntilReady(
            observedFuture,
            std::chrono::seconds(1),
            "wrong-thread getNextLoop did not report runtime_error");
        worker.join();
        pool.stop();
    }

    return 0;
}
