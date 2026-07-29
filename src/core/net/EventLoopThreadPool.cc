#include "gamenet/core/net/EventLoopThreadPool.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace gamenet::net {

namespace {

std::uint64_t stableKeyHash(std::string_view value) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

void validateSelectionPolicy(EventLoopSelectionPolicy policy) {
    switch (policy) {
        case EventLoopSelectionPolicy::RoundRobin:
        case EventLoopSelectionPolicy::LeastConnections:
        case EventLoopSelectionPolicy::QueueLag:
        case EventLoopSelectionPolicy::ConsistentHash:
            return;
    }
    throw std::invalid_argument("unknown EventLoop selection policy");
}

}  // namespace

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop),
      name_(std::move(name)),
      started_(false),
      numThreads_(0),
      next_(0),
      selectionPolicy_(EventLoopSelectionPolicy::RoundRobin) {
}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::setThreadNum(int numThreads) {
    numThreads_ = numThreads;
}

void EventLoopThreadPool::setLoopSelectionPolicy(
    EventLoopSelectionPolicy policy) {
    baseLoop_->assertInLoopThread();
    validateSelectionPolicy(policy);
    if (started_) {
        throw std::logic_error(
            "EventLoopThreadPool selection policy must be configured before start");
    }
    selectionPolicy_ = policy;
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
        connectionLoads_.assign(selectionLoopCount(), 0);
    } catch (...) {
        for (auto& thread : threads_) {
            thread->stop();
        }
        threads_.clear();
        loops_.clear();
        connectionLoads_.clear();
        started_ = false;
        next_ = 0;
        throw;
    }
}

void EventLoopThreadPool::stop() {
    baseLoop_->assertInLoopThread();
    const bool leakedConnectionLoad = std::any_of(
        connectionLoads_.begin(),
        connectionLoads_.end(),
        [](std::size_t load) { return load != 0; });
    for (auto& thread : threads_) {
        thread->stop();
    }
    threads_.clear();
    loops_.clear();
    connectionLoads_.clear();
    started_ = false;
    next_ = 0;
    if (leakedConnectionLoad) {
        throw std::logic_error(
            "EventLoopThreadPool stopped with recorded connection load");
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    return selectLoop();
}

EventLoop* EventLoopThreadPool::selectLoop(std::string_view affinityKey) {
    baseLoop_->assertInLoopThread();
    if (loops_.empty()) {
        return baseLoop_;
    }

    std::size_t index = 0;
    switch (selectionPolicy_) {
        case EventLoopSelectionPolicy::RoundRobin:
            index = selectRoundRobinIndex();
            break;
        case EventLoopSelectionPolicy::LeastConnections:
            index = selectLeastConnectionsIndex();
            break;
        case EventLoopSelectionPolicy::QueueLag:
            index = selectQueueLagIndex();
            break;
        case EventLoopSelectionPolicy::ConsistentHash:
            index = selectConsistentHashIndex(affinityKey);
            break;
    }
    return selectionLoopAt(index);
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}

std::size_t EventLoopThreadPool::selectionLoopCount() const noexcept {
    return loops_.empty() ? 1 : loops_.size();
}

EventLoop* EventLoopThreadPool::selectionLoopAt(std::size_t index) const {
    if (loops_.empty()) {
        if (index != 0) {
            throw std::out_of_range("base EventLoop selection index must be zero");
        }
        return baseLoop_;
    }
    return loops_.at(index);
}

std::size_t EventLoopThreadPool::loopIndex(EventLoop* loop) const {
    if (loops_.empty()) {
        if (loop == baseLoop_) {
            return 0;
        }
        throw std::invalid_argument(
            "EventLoop does not belong to this EventLoopThreadPool");
    }
    const auto found = std::find(loops_.begin(), loops_.end(), loop);
    if (found == loops_.end()) {
        throw std::invalid_argument(
            "EventLoop does not belong to this EventLoopThreadPool");
    }
    return static_cast<std::size_t>(std::distance(loops_.begin(), found));
}

std::size_t EventLoopThreadPool::selectRoundRobinIndex() {
    const std::size_t index = next_ % selectionLoopCount();
    next_ = (index + 1) % selectionLoopCount();
    return index;
}

std::size_t EventLoopThreadPool::selectLeastConnectionsIndex() {
    const std::size_t count = selectionLoopCount();
    const std::size_t start = next_ % count;
    std::size_t best = start;
    for (std::size_t offset = 1; offset < count; ++offset) {
        const std::size_t candidate = (start + offset) % count;
        if (connectionLoads_[candidate] < connectionLoads_[best]) {
            best = candidate;
        }
    }
    next_ = (best + 1) % count;
    return best;
}

std::size_t EventLoopThreadPool::selectQueueLagIndex() {
    const std::size_t count = selectionLoopCount();
    const std::size_t start = next_ % count;
    std::size_t best = start;
    auto bestLoad = selectionLoopAt(best)->pendingFunctorLoadSnapshot();

    for (std::size_t offset = 1; offset < count; ++offset) {
        const std::size_t candidate = (start + offset) % count;
        const auto candidateLoad =
            selectionLoopAt(candidate)->pendingFunctorLoadSnapshot();
        const bool candidateEmpty = candidateLoad.first == 0;
        const bool bestEmpty = bestLoad.first == 0;
        const bool better =
            (candidateEmpty && !bestEmpty) ||
            (candidateEmpty == bestEmpty && !candidateEmpty &&
             (candidateLoad.second > bestLoad.second ||
              (candidateLoad.second == bestLoad.second &&
               candidateLoad.first < bestLoad.first)));
        if (better) {
            best = candidate;
            bestLoad = candidateLoad;
        }
    }

    next_ = (best + 1) % count;
    return best;
}

std::size_t EventLoopThreadPool::selectConsistentHashIndex(
    std::string_view affinityKey) const {
    if (affinityKey.empty()) {
        throw std::invalid_argument(
            "consistent-hash EventLoop selection requires a non-empty affinity key");
    }

    const auto keyHash = stableKeyHash(affinityKey);
    std::size_t best = 0;
    std::uint64_t bestScore = 0;
    for (std::size_t index = 0; index < selectionLoopCount(); ++index) {
        const auto score = mix64(keyHash ^ mix64(index + 1));
        if (index == 0 || score > bestScore) {
            best = index;
            bestScore = score;
        }
    }
    return best;
}

void EventLoopThreadPool::recordConnectionOpened(EventLoop* loop) {
    baseLoop_->assertInLoopThread();
    if (!started_ || connectionLoads_.empty()) {
        throw std::logic_error(
            "cannot record an EventLoop connection before pool start");
    }
    auto& load = connectionLoads_[loopIndex(loop)];
    if (load == std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("EventLoop connection load overflow");
    }
    ++load;
}

void EventLoopThreadPool::recordConnectionClosed(EventLoop* loop) {
    baseLoop_->assertInLoopThread();
    if (!started_ || connectionLoads_.empty()) {
        throw std::logic_error(
            "cannot release an EventLoop connection before pool start");
    }
    auto& load = connectionLoads_[loopIndex(loop)];
    if (load == 0) {
        throw std::logic_error("EventLoop connection load underflow");
    }
    --load;
}

std::size_t EventLoopThreadPool::connectionLoad(EventLoop* loop) const {
    baseLoop_->assertInLoopThread();
    if (!started_ || connectionLoads_.empty()) {
        return 0;
    }
    return connectionLoads_[loopIndex(loop)];
}

}  // namespace gamenet::net
