#pragma once

// EventLoop 是单线程 Reactor 调度核心，负责 poll、事件分发与跨线程任务回流。
// 所有 loop-owned 可变状态都必须只在 owner 线程上访问与销毁。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/TimerId.h"
#include "gamenet/core/net/platform/Wakeup.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gamenet::net {

class Channel;
class Poller;
class TimerQueue;

struct EventLoopOptions {
    std::size_t maxPendingFunctors{65536};
    // Legacy/owner-control queueInLoop calls may consume this bounded reserve;
    // capacity-aware tryQueueInLoop/EventLoopExecutor calls may not.
    std::size_t reservedPendingFunctors{1024};
    std::size_t maxFunctorsPerIteration{1024};

    void validate() const;
};

class EventLoop : private gamenet::base::noncopyable {
public:
    using Functor = std::function<void()>;
    using TimerDuration = std::chrono::steady_clock::duration;

    explicit EventLoop(EventLoopOptions options = {});
    ~EventLoop();

    void loop();
    void quit();

    gamenet::base::Timestamp pollReturnTime() const noexcept;

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    // Non-throwing capacity-aware admission. False means empty callback or
    // saturation; executor admission closure is handled by EventLoopExecutor.
    bool tryQueueInLoop(Functor cb);
    std::size_t pendingFunctorCount() const;
    std::uint64_t rejectedFunctorCount() const noexcept;
    EventLoopExecutor executor() const noexcept;
    void setEventLoopMetricCallback(EventLoopMetricCallback cb);
    void setCallbackExceptionHandler(EventLoopCallbackExceptionHandler cb);
    std::uint64_t callbackExceptionCount() const noexcept;
    TimerId runAt(gamenet::base::Timestamp time, Functor cb);
    TimerId runAfter(TimerDuration delay, Functor cb);
    TimerId runEvery(TimerDuration interval, Functor cb);
    void cancel(TimerId timerId);

    void wakeup();
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    // Used when a connected socket fd moves from Connector to TcpConnection.
    void preserveSocketAssociation(SocketFd sockfd);
    void retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime);
    bool hasChannel(Channel* channel);

    bool isInLoopThread() const noexcept;
    void assertInLoopThread() const;

private:
    struct PendingFunctor {
        Functor functor;
        gamenet::base::Timestamp enqueuedAt;
    };

    void handleRead(gamenet::base::Timestamp receiveTime);
    void doPendingFunctors(std::size_t maxCount);
    bool tryQueueInLoopImpl(Functor cb, bool allowReserve);
    void emitEventLoopMetric(EventLoopMetricSample sample);
    void handleCallbackException(
        EventLoopCallbackSource source,
        std::exception_ptr exception) noexcept;

    using ChannelList = std::vector<Channel*>;

    bool looping_;
    std::atomic<bool> quit_;
    bool eventHandling_;
    std::atomic<bool> callingPendingFunctors_;
    const std::thread::id threadId_;
    std::shared_ptr<EventLoopExecutor::State> executorState_;
    gamenet::base::Timestamp pollReturnTime_;
    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    platform::WakeupFdPair wakeupFds_;
    std::unique_ptr<Channel> wakeupChannel_;
    ChannelList activeChannels_;
    Channel* currentActiveChannel_;
    EventLoopMetricCallback eventLoopMetricCallback_;
    EventLoopCallbackExceptionHandler callbackExceptionHandler_;
    EventLoopOptions options_;
    std::atomic<std::size_t> pendingFunctorPeak_;
    std::atomic<std::uint64_t> wakeupCount_;
    std::atomic<std::uint64_t> rejectedFunctorCount_;
    std::atomic<std::uint64_t> callbackExceptionCount_;
    mutable std::mutex mutex_;
    std::deque<PendingFunctor> pendingFunctors_;
};

}  // namespace gamenet::net
