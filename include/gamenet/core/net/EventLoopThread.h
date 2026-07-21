#pragma once

// EventLoopThread 管理一个后台线程中的单个 EventLoop 生命周期。
// 它负责启动、发布 loop 指针以及在析构时 quit + join。

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace gamenet::net {

class EventLoop;

class EventLoopThread : private gamenet::base::noncopyable {
public:
    EventLoopThread(ThreadInitCallback callback = {}, std::string name = {});
    ~EventLoopThread();

    EventLoop* startLoop();
    void stop();

private:
    void threadFunc();

    EventLoop* loop_;
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr startupException_;
    bool startupComplete_{false};
    ThreadInitCallback callback_;
    std::string name_;
};

}  // namespace gamenet::net
