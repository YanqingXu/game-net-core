// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HP2 实验原型：固定容量、单生产者/单消费者、原地构造的 typed mailbox。
// 仅供默认关闭且非安装的 benchmark/prestudy target 使用，不构成公共 API。

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gamenet::experimental::hp2 {

inline constexpr std::size_t kMailboxCacheLineBytes = 64;

enum class MailboxPushStatus {
    Accepted,
    QueueFull,
    Stopped,
    OwnerUnavailable,
};

struct MailboxPushResult {
    MailboxPushStatus status{MailboxPushStatus::OwnerUnavailable};
    bool notificationIssued{false};
};

enum class MailboxDrainStatus {
    Drained,
    VisitorException,
    ReentrantDrainRejected,
    OwnerUnavailable,
};

struct MailboxDrainResult {
    MailboxDrainStatus status{MailboxDrainStatus::Drained};
    std::size_t processed{};
    std::size_t cancelled{};
    std::size_t remaining{};
    bool needsContinuation{false};
};

struct MailboxSnapshot {
    std::size_t capacity{};
    std::size_t depth{};
    std::size_t depthHighWatermark{};
    std::uint64_t accepted{};
    std::uint64_t processed{};
    std::uint64_t cancelled{};
    std::uint64_t rejectedFull{};
    std::uint64_t rejectedStopped{};
    bool accepting{};

    bool settled() const noexcept {
        return depth == 0 && accepted == processed + cancelled;
    }
};

struct SpscMailboxMatrixPlan {
    std::size_t mailboxCount{};
    std::size_t slotCount{};
    std::size_t totalSlotBytes{};
};

inline SpscMailboxMatrixPlan makeSpscMailboxMatrixPlan(
    std::size_t producerCount,
    std::size_t consumerCount,
    std::size_t directions,
    std::size_t capacity,
    std::size_t slotBytes,
    std::size_t maxMailboxBytes) {
    if (producerCount == 0 || consumerCount == 0 || directions == 0 ||
        capacity == 0 || !std::has_single_bit(capacity) || slotBytes == 0 ||
        maxMailboxBytes == 0) {
        throw std::invalid_argument("SPSC mailbox matrix requires non-zero power-of-two bounds");
    }
    const auto checkedMultiply = [](std::size_t left, std::size_t right) {
        if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
            throw std::overflow_error("SPSC mailbox matrix size overflow");
        }
        return left * right;
    };
    const auto mailboxCount = checkedMultiply(
        checkedMultiply(producerCount, consumerCount), directions);
    const auto slotCount = checkedMultiply(mailboxCount, capacity);
    const auto totalSlotBytes = checkedMultiply(slotCount, slotBytes);
    if (totalSlotBytes > maxMailboxBytes) {
        throw std::length_error("SPSC mailbox matrix exceeds maxMailboxBytes");
    }
    return {
        .mailboxCount = mailboxCount,
        .slotCount = slotCount,
        .totalSlotBytes = totalSlotBytes,
    };
}

template <typename T>
class SpscMailbox {
    static_assert(std::is_nothrow_move_constructible_v<T>);

public:
    explicit SpscMailbox(std::size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          slots_(capacity == 0 ? nullptr : std::make_unique<Slot[]>(capacity)) {
        if (capacity == 0 || !std::has_single_bit(capacity)) {
            throw std::invalid_argument("SpscMailbox capacity must be a positive power of two");
        }
    }

    ~SpscMailbox() {
        beginStop();
        cancelRemaining([](T&&) noexcept {});
    }

    SpscMailbox(const SpscMailbox&) = delete;
    SpscMailbox& operator=(const SpscMailbox&) = delete;

    MailboxPushStatus tryPush(T&& value) noexcept {
        if (!accepting_.load(std::memory_order_acquire)) {
            rejectedStopped_.fetch_add(1, std::memory_order_relaxed);
            return MailboxPushStatus::Stopped;
        }
        const auto tail = producer_.cursor;
        const auto publishedHead = consumer_.published.load(std::memory_order_acquire);
        if (tail - publishedHead >= capacity_) {
            rejectedFull_.fetch_add(1, std::memory_order_relaxed);
            return MailboxPushStatus::QueueFull;
        }

        std::construct_at(slots_[tail & mask_].value(), std::move(value));
        producer_.cursor = tail + 1;
        producer_.published.store(tail + 1, std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);
        updateHighWatermark(static_cast<std::size_t>(tail + 1 - publishedHead));
        return MailboxPushStatus::Accepted;
    }

    template <typename Visitor>
    MailboxDrainResult drain(std::size_t maxItems, Visitor&& visitor) noexcept {
        MailboxDrainResult result;
        if (maxItems == 0) {
            result.remaining = depth();
            result.needsContinuation = result.remaining != 0;
            return result;
        }

        auto head = consumer_.cursor;
        const auto publishedTail = producer_.published.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(publishedTail - head);
        const auto count = std::min(maxItems, available);
        for (std::size_t index = 0; index < count; ++index) {
            auto& slot = slots_[head & mask_];
            T value(std::move(*slot.value()));
            std::destroy_at(slot.value());
            ++head;
            consumer_.cursor = head;
            consumer_.published.store(head, std::memory_order_release);
            try {
                std::invoke(visitor, std::move(value));
                ++result.processed;
                processed_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                ++result.cancelled;
                cancelled_.fetch_add(1, std::memory_order_relaxed);
                accepting_.store(false, std::memory_order_release);
                result.status = MailboxDrainStatus::VisitorException;
                result.remaining = depth();
                result.needsContinuation = result.remaining != 0;
                return result;
            }
        }
        result.remaining = depth();
        result.needsContinuation = result.remaining != 0;
        return result;
    }

    template <typename Visitor>
    MailboxDrainResult cancelRemaining(Visitor&& visitor) noexcept {
        accepting_.store(false, std::memory_order_release);
        MailboxDrainResult result;
        auto head = consumer_.cursor;
        for (;;) {
            const auto publishedTail = producer_.published.load(std::memory_order_acquire);
            if (head == publishedTail) break;
            auto& slot = slots_[head & mask_];
            T value(std::move(*slot.value()));
            std::destroy_at(slot.value());
            ++head;
            consumer_.cursor = head;
            consumer_.published.store(head, std::memory_order_release);
            try {
                std::invoke(visitor, std::move(value));
            } catch (...) {
                // Cancellation observation cannot prevent terminal settlement.
            }
            ++result.cancelled;
            cancelled_.fetch_add(1, std::memory_order_relaxed);
        }
        result.remaining = 0;
        return result;
    }

    void beginStop() noexcept { accepting_.store(false, std::memory_order_release); }

    std::size_t depth() const noexcept {
        const auto tail = producer_.published.load(std::memory_order_acquire);
        const auto head = consumer_.published.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    bool empty() const noexcept { return depth() == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

    MailboxSnapshot snapshot() const noexcept {
        return {
            .capacity = capacity_,
            .depth = depth(),
            .depthHighWatermark = depthHighWatermark_.load(std::memory_order_relaxed),
            .accepted = accepted_.load(std::memory_order_relaxed),
            .processed = processed_.load(std::memory_order_relaxed),
            .cancelled = cancelled_.load(std::memory_order_relaxed),
            .rejectedFull = rejectedFull_.load(std::memory_order_relaxed),
            .rejectedStopped = rejectedStopped_.load(std::memory_order_relaxed),
            .accepting = accepting_.load(std::memory_order_acquire),
        };
    }

private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];

        T* value() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    struct alignas(kMailboxCacheLineBytes) ProducerLine {
        std::uint64_t cursor{};
        std::atomic<std::uint64_t> published{};
    };

    struct alignas(kMailboxCacheLineBytes) ConsumerLine {
        std::uint64_t cursor{};
        std::atomic<std::uint64_t> published{};
    };

    void updateHighWatermark(std::size_t value) noexcept {
        auto observed = depthHighWatermark_.load(std::memory_order_relaxed);
        while (observed < value &&
               !depthHighWatermark_.compare_exchange_weak(
                   observed, value, std::memory_order_relaxed)) {
        }
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;
    ProducerLine producer_;
    ConsumerLine consumer_;
    std::atomic<bool> accepting_{true};
    std::atomic<std::size_t> depthHighWatermark_{};
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> processed_{};
    std::atomic<std::uint64_t> cancelled_{};
    std::atomic<std::uint64_t> rejectedFull_{};
    std::atomic<std::uint64_t> rejectedStopped_{};
};

}  // namespace gamenet::experimental::hp2
