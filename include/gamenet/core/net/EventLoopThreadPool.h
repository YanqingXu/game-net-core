#pragma once

// EventLoopThreadPool 提供 one-loop-per-thread 的扩展模型。
// 它在 base loop 线程中启动 worker loops，并按轮转策略返回下一个 loop。

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"

#include <memory>
#include <string>
#include <vector>

namespace gamenet::net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : private gamenet::base::noncopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    void setThreadNum(int numThreads);
    void start(const ThreadInitCallback& callback = ThreadInitCallback());
    void stop();

    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops() const;

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace gamenet::net
