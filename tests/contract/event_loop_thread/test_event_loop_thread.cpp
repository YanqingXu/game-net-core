#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

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

        GAMENET_TEST_ASSERT(loop != nullptr);

        std::promise<std::thread::id> executedOn;
        auto executedFuture = executedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        loop->queueInLoop([&] {
            GAMENET_TEST_ASSERT(loop->isInLoopThread());
            executedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        const auto status = executedFuture.wait_for(1s);
        GAMENET_TEST_ASSERT(status == std::future_status::ready);
        GAMENET_TEST_ASSERT(executedFuture.get() != callerThread);
    }

    {
        auto loopThread = std::make_unique<gamenet::net::EventLoopThread>();
        gamenet::net::EventLoop* loop = loopThread->startLoop();

        std::promise<void> taskRan;
        auto taskRanFuture = taskRan.get_future();
        std::promise<void> destroyed;
        auto destroyedFuture = destroyed.get_future();

        loop->queueInLoop([&] {
            GAMENET_TEST_ASSERT(loop->isInLoopThread());
            taskRan.set_value();
            loop->quit();
        });

        const auto taskStatus = taskRanFuture.wait_for(1s);
        GAMENET_TEST_ASSERT(taskStatus == std::future_status::ready);

        std::thread destroyer([&] {
            loopThread.reset();
            destroyed.set_value();
        });

        const auto destroyStatus = destroyedFuture.wait_for(1s);
        GAMENET_TEST_ASSERT(destroyStatus == std::future_status::ready);
        destroyer.join();
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> taskStarted;
        auto taskStartedFuture = taskStarted.get_future();
        std::atomic<int> taskFinished{0};

        loop->queueInLoop([&] {
            taskStarted.set_value();
            std::this_thread::sleep_for(30ms);
            ++taskFinished;
        });

        GAMENET_TEST_ASSERT(taskStartedFuture.wait_for(1s) == std::future_status::ready);
        loopThread.stop();
        loopThread.stop();
        GAMENET_TEST_ASSERT(taskFinished.load() == 1);

        gamenet::net::EventLoop* restartedLoop = loopThread.startLoop();
        GAMENET_TEST_ASSERT(restartedLoop != nullptr);

        std::promise<void> restartedTask;
        auto restartedTaskFuture = restartedTask.get_future();
        restartedLoop->queueInLoop([&] {
            GAMENET_TEST_ASSERT(restartedLoop->isInLoopThread());
            restartedTask.set_value();
        });

        GAMENET_TEST_ASSERT(restartedTaskFuture.wait_for(1s) == std::future_status::ready);
        loopThread.stop();
    }

    return 0;
}

