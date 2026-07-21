#include "gamenet/core/net/EventLoopThread.h"

#include "gamenet/core/net/EventLoop.h"

#include <thread>
#include <utility>

namespace gamenet::net {

EventLoopThread::EventLoopThread(ThreadInitCallback callback, std::string name)
    : loop_(nullptr), callback_(std::move(callback)), name_(std::move(name)) {
}

EventLoopThread::~EventLoopThread() {
    stop();
}

EventLoop* EventLoopThread::startLoop() {
    {
        std::lock_guard lock(mutex_);
        if (loop_ != nullptr) {
            return loop_;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    {
        std::lock_guard lock(mutex_);
        startupException_ = nullptr;
        startupComplete_ = false;
    }

    thread_ = std::jthread([this] { threadFunc(); });

    EventLoop* loop = nullptr;
    std::exception_ptr startupException;
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return startupComplete_; });
        loop = loop_;
        startupException = startupException_;
    }
    if (startupException) {
        if (thread_.joinable()) {
            thread_.join();
        }
        std::rethrow_exception(startupException);
    }
    return loop;
}

void EventLoopThread::stop() {
    {
        std::lock_guard lock(mutex_);
        if (loop_ != nullptr) {
            loop_->quit();
        }
    }

    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

void EventLoopThread::threadFunc() {
    EventLoop loop;
    try {
        if (callback_) {
            callback_(&loop);
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        startupException_ = std::current_exception();
        startupComplete_ = true;
        condition_.notify_one();
        return;
    }

    {
        std::lock_guard lock(mutex_);
        loop_ = &loop;
        startupComplete_ = true;
        condition_.notify_one();
    }

    loop.loop();

    {
        std::lock_guard lock(mutex_);
        loop_ = nullptr;
    }
}

}  // namespace gamenet::net
