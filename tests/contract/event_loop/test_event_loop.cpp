#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    {
        gamenet::net::EventLoop loop;
        bool ran = false;
        loop.runInLoop([&] {
            ran = true;
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
        });
        GAMENET_TEST_ASSERT(ran);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::thread::id> executedOn;
        auto future = executedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        std::this_thread::sleep_for(50ms);
        loop->queueInLoop([&] {
            executedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        gamenet::test::waitUntilReady(future, 2s, "cross-thread queueInLoop did not execute");
        GAMENET_TEST_ASSERT(future.get() != callerThread);
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> completed;
        auto future = completed.get_future();
        std::vector<int> order;

        loop->queueInLoop([&] {
            order.push_back(1);
            loop->queueInLoop([&] {
                order.push_back(3);
                GAMENET_TEST_ASSERT(loop->isInLoopThread());
                completed.set_value();
                loop->quit();
            });
            order.push_back(2);
        });

        gamenet::test::waitUntilReady(future, 1s, "nested queueInLoop order did not finish");
        GAMENET_TEST_ASSERT((order == std::vector<int>{1, 2, 3}));
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> completed;
        auto future = completed.get_future();

        std::this_thread::sleep_for(50ms);
        loop->queueInLoop([&] {
            GAMENET_TEST_ASSERT(loop->isInLoopThread());
            completed.set_value();
            loop->quit();
        });

        gamenet::test::waitUntilReady(future, 1s, "cross-thread queueInLoop wakeup did not finish");
    }

    {
        std::promise<gamenet::net::EventLoop*> started;
        auto loopFuture = started.get_future();
        std::promise<void> exited;
        auto exitedFuture = exited.get_future();

        std::thread loopOwner([&] {
            gamenet::net::EventLoop loop;
            started.set_value(&loop);
            loop.loop();
            exited.set_value();
        });

        gamenet::net::EventLoop* loop = loopFuture.get();
        std::this_thread::sleep_for(50ms);
        loop->quit();

        gamenet::test::waitUntilReady(exitedFuture, 1s, "loop owner thread did not exit after quit");
        loopOwner.join();
    }

    {
        gamenet::net::EventLoopThread loopThread;
        gamenet::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> nestedRan;
        auto nestedFuture = nestedRan.get_future();

        loop->queueInLoop([&] {
            loop->queueInLoop([&] { nestedRan.set_value(); });
            loop->quit();
        });

        gamenet::test::waitUntilReady(nestedFuture, 1s, "nested queued functor did not run after quit");
    }

    return 0;
}
