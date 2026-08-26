// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HP3 实验原型：portable slot-index + generation readiness decoder。
// 不拥有 epoll fd/Channel，不进入生产 EpollReadinessPort 或安装表面。

#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gamenet::experimental::hp3 {

inline constexpr std::uint32_t kReadEvent = 0x01;
inline constexpr std::uint32_t kWriteEvent = 0x02;
inline constexpr std::uint32_t kErrorEvent = 0x04;
inline constexpr std::uint32_t kCloseEvent = 0x08;
inline constexpr std::uint64_t kWakeupToken = 0;

enum class SlotDispatchResult {
    Accepted,
    Capacity,
    Conflict,
    Invalid,
    NotRegistered,
    Stopped,
};

struct SlotDispatchOptions {
    std::size_t capacity{4'096};
    std::size_t maxNoticesPerBatch{4'096};
};

struct SlotIdentity {
    int source{-1};
    std::uint32_t slotIndex{};
    std::uint32_t generation{};

    std::uint64_t token() const noexcept;
    bool valid() const noexcept;
    friend bool operator==(const SlotIdentity&, const SlotIdentity&) = default;
};

struct SlotRegistrationResult {
    SlotDispatchResult result{SlotDispatchResult::Invalid};
    SlotIdentity identity{};
};

struct NativeReadinessEvent {
    std::uint64_t token{};
    std::uint32_t events{};
};

struct SlotReadinessNotice {
    SlotIdentity identity{};
    void* target{};
    std::uint32_t events{};
};

struct SlotDecodeMetrics {
    std::uint64_t batchEpoch{};
    std::size_t nativeEvents{};
    std::size_t slotProbes{};
    std::size_t waitHashLookups{};
    std::size_t mergeProbes{};
    std::size_t mergedEvents{};
    std::size_t wakeupEvents{};
    std::size_t malformedEvents{};
    std::size_t staleEvents{};
    std::size_t filteredEvents{};
    std::size_t budgetDroppedEvents{};
    std::size_t deliveredNotices{};
    std::size_t epochResets{};
    bool budgetExhausted{};
};

struct SlotDecodeBatch {
    std::span<const SlotReadinessNotice> notices;
    SlotDecodeMetrics metrics;
};

struct SlotArenaSnapshot {
    std::size_t capacity{};
    std::size_t registrations{};
    std::size_t freeSlots{};
    std::size_t pendingNotices{};
    bool accepting{};
    bool shutdown{};

    bool settled() const noexcept {
        return registrations == 0 && pendingNotices == 0 && shutdown;
    }
};

class EpollSlotDispatchPrototype {
public:
    explicit EpollSlotDispatchPrototype(SlotDispatchOptions options = {});

    EpollSlotDispatchPrototype(const EpollSlotDispatchPrototype&) = delete;
    EpollSlotDispatchPrototype& operator=(const EpollSlotDispatchPrototype&) = delete;

    SlotRegistrationResult registerOrUpdate(
        int source,
        void* target,
        std::uint32_t interests);
    SlotDispatchResult cancel(SlotIdentity identity, void* target);

    SlotDecodeBatch decode(std::span<const NativeReadinessEvent> nativeEvents);
    void finishBatch();

    bool isCurrent(SlotIdentity identity, const void* target) const;
    void beginStop();
    bool tryShutdown();
    SlotArenaSnapshot snapshot() const;

    // Deterministic repository-contract seam; never used by production code.
    void forceBatchEpochForTesting(std::uint32_t epoch);

private:
    struct Slot {
        int source{-1};
        void* target{};
        std::uint32_t interests{};
        std::uint32_t generation{};
        std::uint32_t lastBatchEpoch{};
        std::size_t noticeIndex{};
        bool active{};
    };

    void assertOwnerThread() const;
    std::uint32_t beginBatch();
    static bool validInterests(std::uint32_t interests) noexcept;
    static SlotIdentity unpack(std::uint64_t token) noexcept;

    const std::thread::id ownerThread_;
    SlotDispatchOptions options_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::vector<SlotReadinessNotice> notices_;
    std::unordered_map<int, std::uint32_t> sourceToSlot_;
    std::uint32_t batchEpoch_{};
    bool accepting_{true};
    bool shutdown_{false};
};

}  // namespace gamenet::experimental::hp3
