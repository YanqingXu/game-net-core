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
    // One external control thread must serialize startLoop(), stop(), and
    // destruction. They are not concurrently callable, and destruction/stop
    // must not run from the managed EventLoop thread because teardown joins it.
    EventLoopThread(ThreadInitCallback callback = {}, std::string name = {});
    ~EventLoopThread();

    // The returned pointer is non-owning and remains valid until stop() begins
    // or this EventLoopThread is destroyed. ThreadInitCallback runs on that
    // loop thread; startup exceptions are rethrown here.
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
