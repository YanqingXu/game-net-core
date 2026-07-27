#pragma once

// EventLoop 是单线程 Reactor 调度核心，负责 poll、事件分发与跨线程任务回流。
// 所有 loop-owned 可变状态都必须只在 owner 线程上访问与销毁。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/TimerId.h"
#include "gamenet/core/net/platform/Wakeup.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gamenet::net {

class Channel;
class Connector;
class IocpTcpTransport;
class Poller;
class TcpClient;
class TimerQueue;
namespace detail {
class EventLoopActiveBatchHarness;
class EventLoopControlRegistry;
class EventLoopIocpAssociationHarness;
class EventLoopLifecycleRegistry;
}

// A copyable, non-owning notification capability for one pre-registered
// internal control callback. notify() is thread-safe, non-throwing, and
// coalesces repeated requests into the source's single pending mailbox bit.
class EventLoopControlSource {
public:
    EventLoopControlSource() = default;

    PostResult notify() const noexcept;

private:
    struct State;

    EventLoopControlSource(
        const std::shared_ptr<State>& state,
        std::size_t slot,
        std::uint64_t generation) noexcept;

    std::weak_ptr<State> state_;
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class EventLoop;
};

// A copyable, non-owning signal capability for one runtime lifecycle
// participant. signal() is thread-safe, non-throwing, generation checked, and
// allocates no queue node. Attach/detach remain source-private owner-loop work.
class EventLoopLifecycleSource {
public:
    EventLoopLifecycleSource() = default;

    PostResult signal() const noexcept;

private:
    struct Node;
    struct State;

    EventLoopLifecycleSource(
        const std::shared_ptr<State>& state,
        const std::shared_ptr<Node>& node,
        std::uint64_t generation) noexcept;

    std::weak_ptr<State> state_;
    std::weak_ptr<Node> node_;
    std::uint64_t generation_{0};

    friend class EventLoop;
};

enum class EventLoopPhase {
    Running,
    Quiescing,
    FinalDraining,
    Shutdown,
};

struct EventLoopOptions {
    std::size_t maxPendingFunctors{65536};
    // Legacy/owner-control queueInLoop calls may consume this bounded reserve;
    // capacity-aware tryQueueInLoop/EventLoopExecutor calls may not.
    std::size_t reservedPendingFunctors{1024};
    std::size_t maxFunctorsPerIteration{1024};
    // Fixed registration capacity for internal lifecycle/control sources.
    // Runtime notify() calls allocate no queue nodes and cannot consume normal
    // or reserved pending-functor capacity. Values above 65,536 are rejected
    // before EventLoop allocates its fixed slot/word storage.
    std::size_t maxControlSources{64};
    // Maximum concurrently retained dynamic lifecycle nodes. Node/callback
    // allocation happens only during owner-thread attach.
    std::size_t maxLifecycleNodes{262144};
    // Owner-thread callback budget for one lifecycle drain round.
    std::size_t maxLifecycleCallbacksPerIteration{1024};

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
    std::size_t pendingControlSourceCount() const;
    std::uint64_t controlNotificationCount() const noexcept;
    std::uint64_t mergedControlNotificationCount() const noexcept;
    std::uint64_t rejectedControlNotificationCount() const noexcept;
    std::size_t attachedLifecycleNodeCount() const;
    std::size_t pendingLifecycleNodeCount() const;
    std::uint64_t lifecycleSignalCount() const noexcept;
    std::uint64_t mergedLifecycleSignalCount() const noexcept;
    std::uint64_t rejectedLifecycleSignalCount() const noexcept;
    EventLoopPhase phase() const noexcept;
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
    friend class Connector;
    friend class IocpTcpTransport;
    friend class TcpClient;
    friend class detail::EventLoopActiveBatchHarness;
    friend class detail::EventLoopControlRegistry;
    friend class detail::EventLoopIocpAssociationHarness;
    friend class detail::EventLoopLifecycleRegistry;

    struct PendingFunctor {
        Functor functor;
        gamenet::base::Timestamp enqueuedAt;
    };

    // Source-private infrastructure registration. The access shim lives under
    // src/core and is intentionally absent from installed public headers.
    EventLoopControlSource registerControlSource(Functor cb);
    void unregisterControlSource(const EventLoopControlSource& source);
    EventLoopLifecycleSource attachLifecycleNode(Functor cb);
    void detachLifecycleNode(const EventLoopLifecycleSource& source);
    void forgetSocketAssociation(SocketFd sockfd) noexcept;
    void trackCompletionOperation(void* operation);
    void handleRead(gamenet::base::Timestamp receiveTime);
    void dispatchActiveChannels();
    void retireCurrentChannel(std::unique_ptr<Channel> channel) noexcept;
    void doControlSources();
    void doLifecycleNodes();
    void doPendingFunctors(std::size_t maxCount);
    bool hasPendingControlSources() const;
    bool hasPendingLifecycleNodes() const;
    bool hasPendingFunctors() const;
    bool tryQueueInLoopImpl(Functor cb, bool allowReserve);
    void emitEventLoopMetric(EventLoopMetricSample sample);
    void handleCallbackException(
        EventLoopCallbackSource source,
        std::exception_ptr exception) noexcept;

    using ChannelList = std::vector<Channel*>;

    bool looping_;
    std::atomic<bool> quit_;
    std::atomic<EventLoopPhase> phase_;
    bool eventHandling_;
    std::uint64_t activeBatchEpoch_;
    std::atomic<bool> callingPendingFunctors_;
    const std::thread::id threadId_;
    EventLoopOptions options_;
    std::shared_ptr<EventLoopExecutor::State> executorState_;
    std::shared_ptr<EventLoopControlSource::State> controlState_;
    std::shared_ptr<EventLoopLifecycleSource::State> lifecycleState_;
    std::vector<std::uint64_t> controlDrainWords_;
    gamenet::base::Timestamp pollReturnTime_;
    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    platform::WakeupFdPair wakeupFds_;
    std::unique_ptr<Channel> wakeupChannel_;
    ChannelList activeChannels_;
    Channel* currentActiveChannel_;
    std::unique_ptr<Channel> retiredCurrentChannel_;
    EventLoopMetricCallback eventLoopMetricCallback_;
    EventLoopCallbackExceptionHandler callbackExceptionHandler_;
    std::atomic<std::size_t> pendingFunctorPeak_;
    std::atomic<std::uint64_t> wakeupCount_;
    std::atomic<std::uint64_t> rejectedFunctorCount_;
    std::atomic<std::uint64_t> callbackExceptionCount_;
    mutable std::mutex mutex_;
    std::deque<PendingFunctor> pendingFunctors_;
};

}  // namespace gamenet::net
