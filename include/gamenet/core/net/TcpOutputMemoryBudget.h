#pragma once

// TcpOutputMemoryBudget 是跨连接共享的 TCP 待发送字节预算。
// 热路径仅使用原子记账；对象不拥有连接、EventLoop、payload 或回调。

#include "gamenet/core/base/noncopyable.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gamenet::net {

namespace detail {
class TcpOutputMemoryBudgetHarness;
}

struct TcpOutputMemoryBudgetOptions {
    std::size_t hardLimitBytes{64U * 1024U * 1024U};
    std::size_t recoveryThresholdBytes{48U * 1024U * 1024U};

    // hardLimitBytes must be positive and recoveryThresholdBytes strictly
    // below it; invalid values throw std::invalid_argument.
    void validate() const;
};

struct TcpOutputMemoryBudgetSnapshot {
    std::size_t pendingBytes{0};
    std::size_t peakPendingBytes{0};
    std::uint64_t rejectedReservations{0};
    bool overloaded{false};
};

class TcpOutputMemoryBudget final
    : private gamenet::base::noncopyable {
public:
    explicit TcpOutputMemoryBudget(
        TcpOutputMemoryBudgetOptions options = {});

    TcpOutputMemoryBudgetSnapshot snapshot() const noexcept;
    std::size_t hardLimitBytes() const noexcept;
    std::size_t recoveryThresholdBytes() const noexcept;

private:
    bool tryReserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    void updatePeak(std::size_t candidate) noexcept;

    TcpOutputMemoryBudgetOptions options_;
    std::atomic<std::size_t> pendingBytes_{0};
    std::atomic<std::size_t> peakPendingBytes_{0};
    std::atomic<std::uint64_t> rejectedReservations_{0};
    std::atomic<bool> overloaded_{false};

    friend class TcpConnection;
    friend class detail::TcpOutputMemoryBudgetHarness;
};

}  // namespace gamenet::net
