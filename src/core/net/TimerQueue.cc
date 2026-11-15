// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/core/net/TimerQueue.h"

#include "gamenet/core/net/EventLoop.h"
#include "detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::net {

namespace {

// Observes only pending foreign adds; accepted normal functors own their Timer.
// Keep this source-local so the installed TimerQueue layout/API stays unchanged.
// The index never runs capture destruction under its lock or retains unknown ids.
template <class Timer> class PendingTimerAdds {
public:
    bool publish(const TimerQueue* queue, const std::shared_ptr<Timer>& timer,
                 std::size_t capacity) {
        std::lock_guard lock(mutex_);
        auto [entry, created] = queues_.try_emplace(queue);
        auto& timers = entry->second;
        if (timers.size() >= capacity)
            return false;
        try {
            timers.emplace(timer->sequence, timer);
        } catch (...) {
            if (created)
                queues_.erase(entry);
            throw;
        }
        return true;
    }
    std::shared_ptr<Timer> find(const TimerQueue* queue, std::int64_t sequence) {
        std::lock_guard lock(mutex_);
        const auto entry = queues_.find(queue);
        if (entry == queues_.end())
            return {};
        const auto timer = entry->second.find(sequence);
        return timer == entry->second.end() ? nullptr : timer->second.lock();
    }
    void erase(const TimerQueue* queue, std::int64_t sequence) {
        std::lock_guard lock(mutex_);
        const auto entry = queues_.find(queue);
        if (entry == queues_.end())
            return;
        entry->second.erase(sequence);
        if (entry->second.empty())
            queues_.erase(entry);
    }
    void erase(const TimerQueue* queue) {
        std::lock_guard lock(mutex_);
        queues_.erase(queue);
    }
private:
    std::mutex mutex_;
    std::unordered_map<const TimerQueue*,
        std::unordered_map<std::int64_t, std::weak_ptr<Timer>>> queues_;
};

template <class Timer> PendingTimerAdds<Timer>& pendingTimerAdds() {
    static PendingTimerAdds<Timer> pending;
    return pending;
}

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

gamenet::base::Timestamp nextFixedRateExpiration(
    gamenet::base::Timestamp previousExpiration,
    TimerQueue::Duration interval,
    gamenet::base::Timestamp now,
    std::size_t maxCatchUpCallbacks,
    std::size_t* consecutiveCatchUpCallbacks) {
    auto next = previousExpiration + interval;
    if (next > now) {
        *consecutiveCatchUpCallbacks = 0;
        return next;
    }

    if (*consecutiveCatchUpCallbacks < maxCatchUpCallbacks) {
        ++*consecutiveCatchUpCallbacks;
        return next;
    }

    *consecutiveCatchUpCallbacks = 0;
    const auto missedIntervals = (now - next) / interval;
    next += interval * missedIntervals;
    if (next <= now) {
        if (gamenet::base::Timestamp::max() - next < interval) {
            return gamenet::base::Timestamp::max();
        }
        next += interval;
    }
    return next;
}

}  // namespace

void RepeatingTimerOptions::validate() const {
    switch (mode) {
        case RepeatingTimerMode::FixedDelay:
            if (maxCatchUpCallbacks != 0) {
                throw std::invalid_argument(
                    "fixed-delay repeating timers do not support catch-up callbacks");
            }
            return;
        case RepeatingTimerMode::FixedRate:
            return;
    }
    throw std::invalid_argument("unknown repeating timer mode");
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      nextSequence_(1) {
    // Establish static lifetime before any global EventLoop can finish construction.
    (void)pendingTimerAdds<Timer>();
}

TimerQueue::~TimerQueue() {
    loop_->assertInLoopThread();
    pendingTimerAdds<Timer>().erase(this);
}

TimerScheduleResult TimerQueue::tryAddTimer(
    TimerCallback cb,
    gamenet::base::Timestamp when,
    Duration interval,
    RepeatingTimerOptions options) {
    if (!cb) {
        throw std::invalid_argument("TimerQueue requires a non-empty callback");
    }
    if (interval < Duration::zero()) {
        throw std::invalid_argument("TimerQueue interval cannot be negative");
    }
    options.validate();
    if (interval == Duration::zero() &&
        (options.mode != RepeatingTimerMode::FixedDelay ||
         options.maxCatchUpCallbacks != 0)) {
        throw std::invalid_argument(
            "one-shot timers do not accept repeating timer policy");
    }
    auto timer = std::make_shared<Timer>(
        std::move(cb),
        when,
        interval,
        options,
        nextSequence_.fetch_add(1));
    const TimerId timerId(timer->sequence);

    if (loop_->isInLoopThread()) {
        if (loop_->phase() != EventLoopPhase::Running) {
            return {PostResult::Shutdown, {}};
        }
        addTimerInLoop(std::move(timer));
        return {PostResult::Accepted, timerId};
    }

    auto& pending = pendingTimerAdds<Timer>();
    if (!pending.publish(this, timer,
            detail::EventLoopLifecycleRegistry::normalPendingFunctorCapacity(*loop_))) {
        return {loop_->phase() == EventLoopPhase::Running ? PostResult::QueueFull
                                                         : PostResult::Shutdown, {}};
    }
    PostResult result;
    try {
        result = loop_->executor().post([this, timer] {
            pendingTimerAdds<Timer>().erase(this, timer->sequence);
            addTimerInLoop(timer);
        });
    } catch (...) {
        pending.erase(this, timer->sequence);
        throw;
    }
    if (result != PostResult::Accepted)
        pending.erase(this, timer->sequence);
    return {result, result == PostResult::Accepted ? timerId : TimerId{}};
}

TimerId TimerQueue::addTimer(
    TimerCallback cb,
    gamenet::base::Timestamp when,
    Duration interval,
    RepeatingTimerOptions options) {
    const TimerScheduleResult scheduled = tryAddTimer(
        std::move(cb), when, interval, options);
    switch (scheduled.result) {
        case PostResult::Accepted:
            return scheduled.timerId;
        case PostResult::QueueFull:
            throw std::overflow_error(
                "EventLoop pending functor queue is full for timer admission");
        case PostResult::Shutdown:
            throw std::logic_error(
                "TimerQueue cannot add a timer after EventLoop admission closes");
        case PostResult::OwnerUnavailable:
            throw std::logic_error("TimerQueue owner EventLoop is unavailable");
    }
    throw std::logic_error("unknown timer admission result");
}

PostResult TimerQueue::tryCancel(TimerId timerId) noexcept {
    if (!timerId.valid()) {
        return PostResult::Accepted;
    }

    if (loop_->isInLoopThread()) {
        const bool retirement = detail::EventLoopLifecycleRegistry::consumeTimerRetirement(*loop_);
        if (loop_->phase() != EventLoopPhase::Running && !retirement) {
            return PostResult::Shutdown;
        }
        cancelInLoop(timerId);
        return PostResult::Accepted;
    }

    return loop_->executor().post(
        [this, timerId] { cancelInLoop(timerId); });
}

void TimerQueue::cancel(TimerId timerId) {
    (void)tryCancel(timerId);
}

void TimerQueue::addTimerInLoop(TimerPtr timer) {
    loop_->assertInLoopThread();
    if (!timer->canceled)
        insert(std::move(timer));
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();

    const auto found = timersById_.find(timerId.sequence_);
    if (found == timersById_.end()) {
        if (auto timer = pendingTimerAdds<Timer>().find(this, timerId.sequence_)) {
            timer->canceled = true;
            // Leave only cancelled metadata for the accepted add functor. Clear
            // before destruction so capture reentry cannot observe the callback.
            auto callback = std::exchange(timer->callback, {});
        }
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
            if (timer->options.mode == RepeatingTimerMode::FixedRate) {
                timer->expiration = nextFixedRateExpiration(
                    timer->expiration,
                    timer->interval,
                    now,
                    timer->options.maxCatchUpCallbacks,
                    &timer->consecutiveCatchUpCallbacks);
            } else {
                timer->expiration = now + timer->interval;
                timer->consecutiveCatchUpCallbacks = 0;
            }
            insert(timer);
            continue;
        }
        timersById_.erase(timer->sequence);
    }

}

}  // namespace gamenet::net
