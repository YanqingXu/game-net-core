#pragma once

// EventLoopThreadPool 提供 one-loop-per-thread 的扩展模型。
// 它在 base loop 线程中启动 worker loops，并按显式策略选择连接归属。

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"

#include <memory>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gamenet::net {

class EventLoop;
class EventLoopThread;
class TcpServer;
namespace detail {
class EventLoopThreadPoolSelectionHarness;
}

enum class EventLoopSelectionPolicy {
    RoundRobin,
    LeastConnections,
    QueueLag,
    ConsistentHash,
};

class EventLoopThreadPool : private gamenet::base::noncopyable {
public:
    // baseLoop must be non-null and outlive this pool. Construction,
    // destruction, configuration, selection, start, and stop are base-owner
    // operations.
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    // Configuration and lifecycle control are base-loop-thread-only.
    // Thread count must be non-negative; configuration is immutable while
    // started, and repeated start without an intervening stop is rejected.
    void setThreadNum(int numThreads);
    void setLoopSelectionPolicy(EventLoopSelectionPolicy policy);
    // ThreadInitCallback follows Callbacks.h. In zero-worker mode it executes
    // synchronously on the base loop; any exception rolls start back to Idle
    // and propagates from start().
    void start(const ThreadInitCallback& callback = ThreadInitCallback());
    void stop();

    // Returned loop pointers are non-owning and are invalid once stop() begins.
    // Selection and enumeration are base-loop-thread-only.
    EventLoop* getNextLoop();
    EventLoop* selectLoop(std::string_view affinityKey = {});
    std::vector<EventLoop*> getAllLoops() const;

private:
    std::size_t selectionLoopCount() const noexcept;
    EventLoop* selectionLoopAt(std::size_t index) const;
    std::size_t loopIndex(EventLoop* loop) const;
    std::size_t selectRoundRobinIndex();
    std::size_t selectLeastConnectionsIndex();
    std::size_t selectQueueLagIndex();
    std::size_t selectConsistentHashIndex(std::string_view affinityKey) const;
    void recordConnectionOpened(EventLoop* loop);
    void recordConnectionClosed(EventLoop* loop);
    std::size_t connectionLoad(EventLoop* loop) const;

    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    std::size_t next_;
    EventLoopSelectionPolicy selectionPolicy_;
    std::vector<std::size_t> connectionLoads_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;

    friend class TcpServer;
    friend class detail::EventLoopThreadPoolSelectionHarness;
};

}  // namespace gamenet::net
