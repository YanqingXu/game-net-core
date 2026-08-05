#pragma once

// DeadlineQueue is an owner-loop, bucketed index for large homogeneous
// deadline populations. It stores no user callbacks and owns no target object.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gamenet::net {

class EventLoop;

using DeadlineKey = std::uint64_t;

struct DeadlineToken {
    DeadlineKey key{0};
    std::uint64_t generation{0};

    bool valid() const noexcept {
        return generation != 0;
    }

    friend bool operator==(
        const DeadlineToken&,
        const DeadlineToken&) = default;
};

struct DeadlineQueueOptions {
    std::chrono::steady_clock::duration resolution{
        std::chrono::milliseconds(10)};
    std::size_t maxExpiredPerAdvance{1024};

    // resolution and maxExpiredPerAdvance must both be positive; invalid
    // values throw std::invalid_argument.
    void validate() const;
};

struct DeadlineExpiration {
    DeadlineToken token;
    gamenet::base::Timestamp deadline;
};

struct DeadlineAdvanceResult {
    std::vector<DeadlineExpiration> expired;
    bool readyRemaining{false};
    std::chrono::steady_clock::duration oldestReadyLatency{
        std::chrono::steady_clock::duration::zero()};
};

class DeadlineQueue : private gamenet::base::noncopyable {
public:
    explicit DeadlineQueue(
        EventLoop* ownerLoop,
        DeadlineQueueOptions options = {});
    // Construction, every operation, and destruction are owner-loop-only.
    // The owner pointer must be non-null and outlive this queue.
    ~DeadlineQueue();

    DeadlineToken schedule(
        DeadlineKey key,
        gamenet::base::Timestamp deadline);
    bool cancel(DeadlineToken token);
    DeadlineAdvanceResult advance(
        gamenet::base::Timestamp now);
    void clear();

    std::size_t size() const;
    std::optional<gamenet::base::Timestamp> nextBucketDeadline() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gamenet::net
