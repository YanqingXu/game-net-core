#pragma once

// TimerQueue 为单个 EventLoop 提供 poll-timeout 驱动的定时任务能力。
// 它维护 one-shot / repeating timer，并确保回调始终在 owner loop 线程执行。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <exception>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamenet::net {

class Channel;
class EventLoop;

class TimerQueue : private gamenet::base::noncopyable {
public:
    using TimerCallback = std::function<void()>;
    using Duration = std::chrono::steady_clock::duration;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerId addTimer(TimerCallback cb, gamenet::base::Timestamp when, Duration interval = Duration::zero());
    void cancel(TimerId timerId);

private:
    struct Timer {
        Timer(TimerCallback timerCallback, gamenet::base::Timestamp expirationTime, Duration repeatInterval, std::int64_t id)
            : callback(std::move(timerCallback)),
              expiration(expirationTime),
              interval(repeatInterval),
              sequence(id) {
        }

        bool repeat() const noexcept {
            return interval > Duration::zero();
        }

        TimerCallback callback;
        gamenet::base::Timestamp expiration;
        Duration interval;
        std::int64_t sequence;
        bool canceled{false};
        bool inQueue{true};
    };

    using TimerPtr = std::shared_ptr<Timer>;
    using TimerKey = std::pair<gamenet::base::Timestamp, std::int64_t>;
    using TimerMap = std::map<TimerKey, TimerPtr>;

    void addTimerInLoop(TimerPtr timer);
    void cancelInLoop(TimerId timerId);
    int pollTimeoutMs(int defaultTimeoutMs) const;
    std::vector<std::exception_ptr> handleExpired(gamenet::base::Timestamp now);

    bool insert(TimerPtr timer);
    std::vector<TimerPtr> getExpired(gamenet::base::Timestamp now);
    void reset(const std::vector<TimerPtr>& expired, gamenet::base::Timestamp now);

    EventLoop* loop_;
    std::atomic<std::int64_t> nextSequence_;
    TimerMap timers_;
    std::unordered_map<std::int64_t, TimerPtr> timersById_;

    friend class EventLoop;
};

}  // namespace gamenet::net
