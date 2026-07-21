#include "gamenet/core/net/EventLoopThreadPool.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include <memory>
#include <string>
#include <utility>

namespace gamenet::net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop), name_(std::move(name)), started_(false), numThreads_(0), next_(0) {
}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::setThreadNum(int numThreads) {
    numThreads_ = numThreads;
}

void EventLoopThreadPool::start(const ThreadInitCallback& callback) {
    baseLoop_->assertInLoopThread();
    started_ = true;

    try {
        for (int i = 0; i < numThreads_; ++i) {
            auto thread = std::make_unique<EventLoopThread>(callback, name_ + std::to_string(i));
            loops_.push_back(thread->startLoop());
            threads_.push_back(std::move(thread));
        }

        if (numThreads_ == 0 && callback) {
            callback(baseLoop_);
        }
    } catch (...) {
        for (auto& thread : threads_) {
            thread->stop();
        }
        threads_.clear();
        loops_.clear();
        started_ = false;
        next_ = 0;
        throw;
    }
}

void EventLoopThreadPool::stop() {
    baseLoop_->assertInLoopThread();
    for (auto& thread : threads_) {
        thread->stop();
    }
    threads_.clear();
    loops_.clear();
    started_ = false;
    next_ = 0;
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    EventLoop* loop = baseLoop_;
    if (!loops_.empty()) {
        loop = loops_[static_cast<std::size_t>(next_)];
        ++next_;
        if (next_ >= static_cast<int>(loops_.size())) {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}

}  // namespace gamenet::net
