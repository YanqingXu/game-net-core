#pragma once

// TimerQueue 为单个 EventLoop 提供 poll-timeout 驱动的定时任务能力。
// 它维护 one-shot / repeating timer，并确保回调始终在 owner loop 线程执行。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/TimerId.h"
#include "gamenet/core/net/TimerOptions.h"

#include <atomic>
#include <chrono>
#include <cstddef>
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

    TimerId addTimer(
        TimerCallback cb,
        gamenet::base::Timestamp when,
        Duration interval = Duration::zero(),
        RepeatingTimerOptions options = {});
    void cancel(TimerId timerId);

private:
    struct ExpiredResult {
        std::vector<std::exception_ptr> exceptions;
        std::size_t drained{0};
        std::size_t remaining{0};
        Duration oldestReadyLatency{Duration::zero()};
    };

    struct Timer {
        Timer(
            TimerCallback timerCallback,
            gamenet::base::Timestamp expirationTime,
            Duration repeatInterval,
            RepeatingTimerOptions repeatingOptions,
            std::int64_t id)
            : callback(std::move(timerCallback)),
              expiration(expirationTime),
              interval(repeatInterval),
              options(repeatingOptions),
              sequence(id) {
        }

        bool repeat() const noexcept {
            return interval > Duration::zero();
        }

        TimerCallback callback;
        gamenet::base::Timestamp expiration;
        Duration interval;
        RepeatingTimerOptions options;
        std::size_t consecutiveCatchUpCallbacks{0};
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
    ExpiredResult handleExpired(
        gamenet::base::Timestamp now,
        std::size_t maxCount);

    bool insert(TimerPtr timer);
    std::vector<TimerPtr> getExpired(
        gamenet::base::Timestamp now,
        std::size_t maxCount);
    void reset(const std::vector<TimerPtr>& expired, gamenet::base::Timestamp now);

    EventLoop* loop_;
    std::atomic<std::int64_t> nextSequence_;
    TimerMap timers_;
    std::unordered_map<std::int64_t, TimerPtr> timersById_;

    friend class EventLoop;
};

}  // namespace gamenet::net
