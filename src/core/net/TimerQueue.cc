#include "gamenet/core/net/TimerQueue.h"

#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::net {

namespace {

int ceilMilliseconds(std::chrono::steady_clock::duration delay) {
    using namespace std::chrono;

    if (delay <= steady_clock::duration::zero()) {
        return 0;
    }

    auto millisecondsPart = duration_cast<milliseconds>(delay);
    if (millisecondsPart < delay) {
        ++millisecondsPart;
    }
    const auto capped = std::min<std::int64_t>(
        millisecondsPart.count(),
        std::numeric_limits<int>::max());
    return static_cast<int>(std::max<std::int64_t>(1, capped));
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      nextSequence_(1) {
}

TimerQueue::~TimerQueue() {
    loop_->assertInLoopThread();
}

TimerId TimerQueue::addTimer(TimerCallback cb, gamenet::base::Timestamp when, Duration interval) {
    auto timer = std::make_shared<Timer>(std::move(cb), when, interval, nextSequence_.fetch_add(1));
    const TimerId timerId(timer->sequence);

    if (loop_->isInLoopThread()) {
        addTimerInLoop(std::move(timer));
    } else {
        loop_->runInLoop([this, timer] { addTimerInLoop(timer); });
    }

    return timerId;
}

void TimerQueue::cancel(TimerId timerId) {
    if (!timerId.valid()) {
        return;
    }

    if (loop_->isInLoopThread()) {
        cancelInLoop(timerId);
    } else {
        loop_->runInLoop([this, timerId] { cancelInLoop(timerId); });
    }
}

void TimerQueue::addTimerInLoop(TimerPtr timer) {
    loop_->assertInLoopThread();
    insert(std::move(timer));
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();

    const auto found = timersById_.find(timerId.sequence_);
    if (found == timersById_.end()) {
        return;
    }

    const auto& timer = found->second;
    timer->canceled = true;

    if (timer->inQueue) {
        timers_.erase(TimerKey{timer->expiration, timer->sequence});
        timer->inQueue = false;
        timersById_.erase(found);

        return;
    }

    timersById_.erase(found);
}

int TimerQueue::pollTimeoutMs(int defaultTimeoutMs) const {
    loop_->assertInLoopThread();
    if (timers_.empty()) {
        return defaultTimeoutMs;
    }

    const int nextTimerMs = ceilMilliseconds(timers_.begin()->second->expiration - gamenet::base::now());
    return std::min(defaultTimeoutMs, nextTimerMs);
}

TimerQueue::ExpiredResult TimerQueue::handleExpired(
    gamenet::base::Timestamp now,
    std::size_t maxCount) {
    loop_->assertInLoopThread();
    if (timers_.empty() || timers_.begin()->second->expiration > now) {
        return {};
    }

    ExpiredResult result;
    result.oldestReadyLatency =
        now - timers_.begin()->second->expiration;
    auto expired = getExpired(now, maxCount);
    result.drained = expired.size();
    result.exceptions.reserve(expired.size());
    for (const auto& timer : expired) {
        if (!timer->canceled) {
            try {
                timer->callback();
            } catch (...) {
                timer->canceled = true;
                result.exceptions.push_back(std::current_exception());
            }
        }
    }
    reset(expired, gamenet::base::now());

    const TimerKey sentry{
        gamenet::base::now(),
        std::numeric_limits<std::int64_t>::max()};
    result.remaining = static_cast<std::size_t>(
        std::distance(timers_.begin(), timers_.upper_bound(sentry)));
    return result;
}

bool TimerQueue::insert(TimerPtr timer) {
    loop_->assertInLoopThread();

    const bool earliestChanged = timers_.empty() || TimerKey{timer->expiration, timer->sequence} < timers_.begin()->first;
    timer->inQueue = true;
    timer->canceled = false;
    timersById_[timer->sequence] = timer;
    timers_.emplace(TimerKey{timer->expiration, timer->sequence}, timer);

    return earliestChanged;
}

std::vector<TimerQueue::TimerPtr> TimerQueue::getExpired(
    gamenet::base::Timestamp now,
    std::size_t maxCount) {
    std::vector<TimerPtr> expired;
    const TimerKey sentry{now, std::numeric_limits<std::int64_t>::max()};
    const auto end = timers_.upper_bound(sentry);

    auto it = timers_.begin();
    for (; it != end && expired.size() < maxCount; ++it) {
        it->second->inQueue = false;
        expired.push_back(it->second);
    }

    timers_.erase(timers_.begin(), it);
    return expired;
}

void TimerQueue::reset(const std::vector<TimerPtr>& expired, gamenet::base::Timestamp now) {
    for (const auto& timer : expired) {
        if (timer->repeat() && !timer->canceled && timersById_.contains(timer->sequence)) {
            timer->expiration = now + timer->interval;
            insert(timer);
            continue;
        }
        timersById_.erase(timer->sequence);
    }

}

}  // namespace gamenet::net
