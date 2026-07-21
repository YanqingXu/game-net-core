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
};

using EventLoopMetricCallback = std::function<void(const EventLoopMetricSample&)>;

}  // namespace gamenet::net
