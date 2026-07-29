#pragma once

// Lightweight EventLoop metrics kept inside core without importing TCP/game hooks.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace gamenet::net {

class EventLoop;

enum class EventLoopMetricEvent {
    PendingFunctorsDrained,
    WakeupHandled,
    // Appended to preserve existing event values within the 0.3 source line.
    ControlSourcesDrained,
    ActiveChannelsDrained,
    TimersDrained,
    LifecycleNodesDrained,
    IocpCompletionPacketsDrained,
};

struct EventLoopMetricSample {
    using Duration = std::chrono::steady_clock::duration;

    EventLoopMetricEvent event{EventLoopMetricEvent::PendingFunctorsDrained};
    EventLoop* loop{nullptr};
    std::size_t pendingFunctors{0};
    std::size_t pendingFunctorPeak{0};
    std::uint64_t wakeupCount{0};
    std::uint64_t rejectedFunctors{0};
    std::uint64_t callbackExceptions{0};
    Duration oldestPendingLatency{Duration::zero()};
    // New control-lane observations stay append-only so existing positional
    // aggregate initialization retains the 0.3 field meanings.
    std::size_t pendingControlSources{0};
    std::size_t pendingControlSourcePeak{0};
    std::uint64_t controlNotifications{0};
    std::uint64_t mergedControlNotifications{0};
    std::uint64_t rejectedControlNotifications{0};
    // Append-only scheduler observations shared by every budgeted phase.
    std::size_t drainedWork{0};
    std::size_t remainingWork{0};
    Duration oldestReadyLatency{Duration::zero()};
    bool budgetExhausted{false};
};

using EventLoopMetricCallback = std::function<void(const EventLoopMetricSample&)>;

}  // namespace gamenet::net
