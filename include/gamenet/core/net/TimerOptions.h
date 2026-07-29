#pragma once

// TimerOptions defines explicit cadence semantics for repeating EventLoop timers.
// Catch-up is always bounded and never changes owner-loop callback affinity.

#include <cstddef>

namespace gamenet::net {

enum class RepeatingTimerMode {
    // Schedule the next callback relative to completion of the current callback.
    FixedDelay,
    // Preserve the original cadence and optionally replay a bounded number of
    // missed callbacks before skipping to the next future cadence point.
    FixedRate,
};

struct RepeatingTimerOptions {
    RepeatingTimerMode mode{RepeatingTimerMode::FixedDelay};
    // FixedRate only. Zero skips every missed cadence point. A positive value
    // permits at most this many consecutive catch-up callbacks.
    std::size_t maxCatchUpCallbacks{0};

    void validate() const;
};

}  // namespace gamenet::net
