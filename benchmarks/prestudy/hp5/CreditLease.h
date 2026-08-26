// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace gamenet::experimental::hp5 {

struct SharedCreditSnapshot {
    std::size_t reservedBytes{};
    std::size_t peakReservedBytes{};
    std::uint64_t rejectedReservations{};
    std::uint64_t modeledAtomicMutations{};
};

class SharedCreditBudget final {
public:
    explicit SharedCreditBudget(std::size_t hardLimitBytes);

    SharedCreditBudget(const SharedCreditBudget&) = delete;
    SharedCreditBudget& operator=(const SharedCreditBudget&) = delete;

    bool tryReserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    SharedCreditSnapshot snapshot() const noexcept;
    std::size_t hardLimitBytes() const noexcept;

private:
    void updatePeak(std::size_t candidate) noexcept;

    std::size_t hardLimitBytes_{};
    std::atomic<std::size_t> reservedBytes_{0};
    std::atomic<std::size_t> peakReservedBytes_{0};
    std::atomic<std::uint64_t> rejectedReservations_{0};
    std::atomic<std::uint64_t> modeledAtomicMutations_{0};
};

struct ConnectionCreditHandle {
    std::uint32_t index{};
    std::uint32_t generation{};

    bool valid() const noexcept { return generation != 0; }
};

enum class CreditStatus {
    Accepted,
    NoConnectionSlot,
    ConnectionLimit,
    LoopLimit,
    ServerLimit,
    GlobalLimit,
    Stopped,
    StaleHandle,
    WrongOwner,
    PendingBytes,
    Invalid,
};

struct ConnectionRegistration {
    CreditStatus status{CreditStatus::Invalid};
    ConnectionCreditHandle handle{};
};

struct LoopCreditLeaseOptions {
    std::size_t maxConnections{64};
    std::size_t loopHardLimitBytes{4U * 1024U * 1024U};
    std::size_t leaseQuantumBytes{64U * 1024U};
    std::size_t retainedIdleCreditBytes{64U * 1024U};

    void validate() const;
};

struct LoopCreditLeaseSnapshot {
    std::size_t activeConnections{};
    std::size_t usedBytes{};
    std::size_t leasedBytes{};
    std::uint64_t acceptedBytes{};
    std::uint64_t releasedBytes{};
    std::uint64_t discardedBytes{};
    std::uint64_t refillCount{};
    std::uint64_t returnCount{};
    bool stopped{};

    bool settled() const noexcept {
        return stopped && activeConnections == 0 && usedBytes == 0 &&
            leasedBytes == 0 &&
            acceptedBytes == releasedBytes + discardedBytes;
    }
};

class LoopCreditLease final {
public:
    LoopCreditLease(
        LoopCreditLeaseOptions options,
        std::shared_ptr<SharedCreditBudget> serverBudget,
        std::shared_ptr<SharedCreditBudget> globalBudget);
    ~LoopCreditLease();

    LoopCreditLease(const LoopCreditLease&) = delete;
    LoopCreditLease& operator=(const LoopCreditLease&) = delete;

    ConnectionRegistration registerConnection(std::size_t hardLimitBytes);
    CreditStatus retireConnection(ConnectionCreditHandle handle);
    CreditStatus tryReserve(ConnectionCreditHandle handle, std::size_t bytes);
    CreditStatus release(ConnectionCreditHandle handle, std::size_t bytes);
    CreditStatus cancel(ConnectionCreditHandle handle);
    CreditStatus trimIdleCredit();
    CreditStatus beginStop();
    CreditStatus cancelAll();
    bool tryShutdown() const noexcept;
    LoopCreditLeaseSnapshot snapshot() const noexcept;

private:
    struct ConnectionSlot {
        std::uint32_t generation{1};
        std::size_t hardLimitBytes{};
        std::size_t pendingBytes{};
        bool active{};
    };

    bool isOwner() const noexcept;
    ConnectionSlot* resolve(ConnectionCreditHandle handle) noexcept;
    CreditStatus refill(std::size_t bytes);
    void returnCredit(std::size_t bytes) noexcept;
    static std::uint32_t nextGeneration(std::uint32_t generation) noexcept;

    LoopCreditLeaseOptions options_;
    std::shared_ptr<SharedCreditBudget> serverBudget_;
    std::shared_ptr<SharedCreditBudget> globalBudget_;
    std::vector<ConnectionSlot> slots_;
    std::thread::id owner_;
    std::size_t activeConnections_{};
    std::size_t usedBytes_{};
    std::size_t leasedBytes_{};
    std::uint64_t acceptedBytes_{};
    std::uint64_t releasedBytes_{};
    std::uint64_t discardedBytes_{};
    std::uint64_t refillCount_{};
    std::uint64_t returnCount_{};
    bool stopped_{};
};

}  // namespace gamenet::experimental::hp5
