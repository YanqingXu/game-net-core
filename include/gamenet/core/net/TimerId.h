#pragma once

// TimerId 是 TimerQueue 对外暴露的取消句柄。
// 它只承载稳定的序号身份，不暴露内部定时器对象地址。

#include "gamenet/core/net/PostResult.h"

#include <cstdint>

namespace gamenet::net {

class TimerQueue;

class TimerId {
public:
    TimerId() noexcept = default;

    bool valid() const noexcept {
        return sequence_ != 0;
    }

private:
    explicit TimerId(std::int64_t sequence) noexcept : sequence_(sequence) {
    }

    std::int64_t sequence_{0};

    friend class TimerQueue;
};

// Typed timer admission. A valid timerId is present only when result is
// Accepted. Accepted commits timer metadata to owner-loop work; it does not
// keep the EventLoop alive or require a future deadline to fire after shutdown.
struct TimerScheduleResult {
    PostResult result{PostResult::OwnerUnavailable};
    TimerId timerId{};
};

}  // namespace gamenet::net
