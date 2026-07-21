#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/EventLoopThreadPool.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/FutureTest.h"
#include "support/TestAssert.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#else
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

namespace {

struct CallbackReadablePair {
    gamenet::net::SocketFd readFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd writeFd{gamenet::net::kInvalidSocket};

    CallbackReadablePair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        readFd = fds[0];
        writeFd = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        readFd = fds[0];
        writeFd = fds[1];
#endif
    }

    ~CallbackReadablePair() {
        gamenet::net::sockets::close(readFd);
        gamenet::net::sockets::close(writeFd);
    }
};

bool containsCallbackSource(
    const std::vector<gamenet::net::EventLoopCallbackSource>& sources,
    gamenet::net::EventLoopCallbackSource source) {
    return std::find(sources.begin(), sources.end(), source) != sources.end();
}

}  // namespace

int main() {
    gamenet::net::EventLoopExecutor expiredExecutor;
    {
        gamenet::net::EventLoop loop;
        expiredExecutor = loop.executor();
        GAMENET_TEST_ASSERT(expiredExecutor.id() != 0);
        GAMENET_TEST_ASSERT(expiredExecutor.available());
        GAMENET_TEST_ASSERT(expiredExecutor.isInOwnerThread());
    }
    GAMENET_TEST_ASSERT(!expiredExecutor.available());
    GAMENET_TEST_ASSERT(!expiredExecutor.isInOwnerThread());
    GAMENET_TEST_ASSERT(!expiredExecutor.tryQueue([] {}));

    {
        gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
            .maxPendingFunctors = 3,
            .reservedPendingFunctors = 0,
            .maxFunctorsPerIteration = 2,
        });
        const auto executor = loop.executor();
        std::vector<int> order;
        bool yieldedToTimer = false;

        GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] {
            order.push_back(1);
            loop.runAfter(0ms, [&] {
                yieldedToTimer = true;
                order.push_back(3);
            });
        }));
        GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] { order.push_back(2); }));
        GAMENET_TEST_ASSERT(loop.tryQueueInLoop([&] {
            GAMENET_TEST_ASSERT(yieldedToTimer);
            order.push_back(4);
            loop.quit();
        }));

        GAMENET_TEST_ASSERT(!executor.tryQueue([] {}));
        bool queueFullWasExplicit = false;
        try {
            loop.queueInLoop([] {});
        } catch (const std::overflow_error&) {
            queueFullWasExplicit = true;
        }
        GAMENET_TEST_ASSERT(queueFullWasExplicit);
        GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 3);
        GAMENET_TEST_ASSERT(loop.rejectedFunctorCount() == 2);

        // Avoid the initial poll wait while preserving the first pending batch.
        loop.runAfter(0ms, [] {});
        loop.loop();

        GAMENET_TEST_ASSERT((order == std::vector<int>{1, 2, 3, 4}));
        GAMENET_TEST_ASSERT(loop.pendingFunctorCount() == 0);
    }

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
        const auto executor = loop->executor();

        std::promise<std::thread::id> executedOn;
        auto future = executedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        std::this_thread::sleep_for(50ms);
        GAMENET_TEST_ASSERT(executor.tryQueue([&] {
            executedOn.set_value(std::this_thread::get_id());
            loop->quit();
        }));

        gamenet::test::waitUntilReady(future, 2s, "cross-thread queueInLoop did not execute");
        GAMENET_TEST_ASSERT(future.get() != callerThread);
        loopThread.stop();
        GAMENET_TEST_ASSERT(!executor.available());
        GAMENET_TEST_ASSERT(!executor.tryQueue([] {}));
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
        const auto executor = loop->executor();

        std::promise<void> nestedRan;
        auto nestedFuture = nestedRan.get_future();

        loop->queueInLoop([&] {
            loop->queueInLoop([&] {
                GAMENET_TEST_ASSERT(executor.isInOwnerThread());
                GAMENET_TEST_ASSERT(!executor.available());
                nestedRan.set_value();
            });
            loop->quit();
        });

        gamenet::test::waitUntilReady(nestedFuture, 1s, "nested queued functor did not run after quit");
        loopThread.stop();
        GAMENET_TEST_ASSERT(!executor.available());
        GAMENET_TEST_ASSERT(!executor.isInOwnerThread());
    }

    {
        gamenet::net::EventLoop loop;
        CallbackReadablePair pair;
        gamenet::net::Channel channel(&loop, pair.readFd);
        std::vector<gamenet::net::EventLoopCallbackSource> sources;
        bool metricThrew = false;
        bool laterFunctorRan = false;
        bool laterTimerRan = false;

        loop.setCallbackExceptionHandler(
            [&](const gamenet::net::EventLoopCallbackException& failure) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(failure.exception != nullptr);
                sources.push_back(failure.source);
                return gamenet::net::EventLoopCallbackExceptionAction::Continue;
            });
        loop.setEventLoopMetricCallback([&](const gamenet::net::EventLoopMetricSample&) {
            if (!metricThrew) {
                metricThrew = true;
                throw std::runtime_error("metric callback failure");
            }
        });

#ifdef _WIN32
        gamenet::net::IocpOperation readOperation{};
        readOperation.kind = gamenet::net::IocpOperationKind::Read;
        readOperation.channel = &channel;
        std::array<char, 8> readStorage{};
        WSABUF readBuffer{};
#endif

        channel.setReadCallback([&](gamenet::base::Timestamp) {
#ifdef _WIN32
            GAMENET_TEST_ASSERT(readOperation.error == 0);
#else
            char byte = 0;
            GAMENET_TEST_ASSERT(gamenet::net::sockets::read(pair.readFd, &byte, 1) == 1);
#endif
            channel.disableAll();
            channel.remove();
            throw std::runtime_error("channel callback failure");
        });
        channel.enableReading();

#ifdef _WIN32
        readBuffer.buf = readStorage.data();
        readBuffer.len = static_cast<ULONG>(readStorage.size());
        DWORD bytes = 0;
        DWORD flags = 0;
        const int rc = ::WSARecv(
            pair.readFd,
            &readBuffer,
            1,
            &bytes,
            &flags,
            &readOperation.overlapped,
            nullptr);
        GAMENET_TEST_ASSERT(
            rc == 0 || gamenet::net::sockets::lastError() == WSA_IO_PENDING);
#endif

        loop.queueInLoop([] { throw std::runtime_error("pending callback failure"); });
        loop.queueInLoop([&] { laterFunctorRan = true; });
        loop.runAfter(0ms, [] { throw std::runtime_error("timer callback failure"); });
        loop.runAfter(30ms, [&] {
            laterTimerRan = true;
            GAMENET_TEST_ASSERT(laterFunctorRan);
            GAMENET_TEST_ASSERT(metricThrew);
            GAMENET_TEST_ASSERT(containsCallbackSource(
                sources,
                gamenet::net::EventLoopCallbackSource::ChannelEvent));
            GAMENET_TEST_ASSERT(containsCallbackSource(
                sources,
                gamenet::net::EventLoopCallbackSource::Timer));
            GAMENET_TEST_ASSERT(containsCallbackSource(
                sources,
                gamenet::net::EventLoopCallbackSource::PendingFunctor));
            GAMENET_TEST_ASSERT(containsCallbackSource(
                sources,
                gamenet::net::EventLoopCallbackSource::Metric));
            GAMENET_TEST_ASSERT(loop.callbackExceptionCount() == 4);
            loop.quit();
        });

        const char byte = 'x';
        GAMENET_TEST_ASSERT(gamenet::net::sockets::write(pair.writeFd, &byte, 1) == 1);
        loop.loop();
        GAMENET_TEST_ASSERT(laterTimerRan);
        GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));
    }

    {
        gamenet::net::EventLoop loop;
        bool acceptedAfterFailureRan = false;
        loop.setCallbackExceptionHandler(
            [](const gamenet::net::EventLoopCallbackException&) {
                return gamenet::net::EventLoopCallbackExceptionAction::Quit;
            });
        loop.queueInLoop([] { throw std::runtime_error("request quit"); });
        loop.queueInLoop([&] { acceptedAfterFailureRan = true; });
        loop.runAfter(0ms, [] {});
        loop.loop();
        GAMENET_TEST_ASSERT(acceptedAfterFailureRan);
        GAMENET_TEST_ASSERT(loop.callbackExceptionCount() == 1);
    }

    {
        gamenet::net::EventLoop loop;
        bool acceptedAfterHandlerFailureRan = false;
        loop.setCallbackExceptionHandler(
            [](const gamenet::net::EventLoopCallbackException&) ->
                gamenet::net::EventLoopCallbackExceptionAction {
                throw std::runtime_error("exception handler failure");
            });
        loop.queueInLoop([] { throw std::runtime_error("reported failure"); });
        loop.queueInLoop([&] { acceptedAfterHandlerFailureRan = true; });
        loop.runAfter(0ms, [] {});
        loop.loop();
        GAMENET_TEST_ASSERT(acceptedAfterHandlerFailureRan);
        GAMENET_TEST_ASSERT(loop.callbackExceptionCount() == 1);
    }

    {
        gamenet::net::EventLoopThread loopThread(
            [](gamenet::net::EventLoop*) { throw std::runtime_error("thread init failure"); });
        bool startupFailureObserved = false;
        try {
            (void)loopThread.startLoop();
        } catch (const std::runtime_error& error) {
            startupFailureObserved = std::string_view(error.what()) == "thread init failure";
        }
        GAMENET_TEST_ASSERT(startupFailureObserved);
    }

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::net::EventLoopThreadPool pool(&baseLoop, "callback-startup-");
        pool.setThreadNum(2);
        int initialized = 0;
        bool poolFailureObserved = false;
        try {
            pool.start([&](gamenet::net::EventLoop*) {
                if (++initialized == 2) {
                    throw std::runtime_error("second worker init failure");
                }
            });
        } catch (const std::runtime_error& error) {
            poolFailureObserved = std::string_view(error.what()) ==
                "second worker init failure";
        }
        GAMENET_TEST_ASSERT(poolFailureObserved);
        const auto loops = pool.getAllLoops();
        GAMENET_TEST_ASSERT(loops.size() == 1);
        GAMENET_TEST_ASSERT(loops.front() == &baseLoop);
    }

    return 0;
}
