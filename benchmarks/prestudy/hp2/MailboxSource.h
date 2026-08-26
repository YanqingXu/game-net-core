// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HP2 实验原型：generation-safe producer handle 与 empty→non-empty 合并通知。
// 它不注册生产 EventLoop lane，也不进入安装或导出表面。

#include "hp2/SpscMailbox.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace gamenet::experimental::hp2 {

enum class NotificationClearPhase {
    BeforeClear,
    AfterClear,
};

struct MailboxSourceSnapshot {
    MailboxSnapshot mailbox;
    std::uint64_t generation{};
    std::uint64_t physicalNotifications{};
    std::uint64_t mergedNotifications{};
    std::uint64_t ownerContinuations{};
    std::uint64_t selfRearms{};
    std::uint64_t reentrantDrainRejections{};
    std::uint64_t notifierExceptions{};
    std::uint64_t activeProducerCalls{};
    bool notificationPending{};
    bool ownerAvailable{};

    bool settled() const noexcept {
        return mailbox.settled() && activeProducerCalls == 0 &&
            !notificationPending;
    }
};

template <typename T>
class SpscMailboxSource {
private:
    struct State;

public:
    class ProducerHandle {
    public:
        ProducerHandle() = default;

        MailboxPushResult tryPush(T&& value) const noexcept {
            const auto state = state_;
            if (!state || generation_ == 0) {
                return {.status = MailboxPushStatus::OwnerUnavailable};
            }
            state->activeProducerCalls.fetch_add(1, std::memory_order_acq_rel);
            struct ActiveGuard {
                State& state;
                ~ActiveGuard() {
                    state.activeProducerCalls.fetch_sub(1, std::memory_order_acq_rel);
                }
            } guard{*state};

            if (!state->ownerAvailable.load(std::memory_order_acquire) ||
                state->generation.load(std::memory_order_acquire) != generation_) {
                return {.status = MailboxPushStatus::OwnerUnavailable};
            }

            const auto status = state->mailbox.tryPush(std::move(value));
            if (status != MailboxPushStatus::Accepted) return {.status = status};

            bool issued = false;
            if (!state->notificationPending.exchange(true, std::memory_order_acq_rel)) {
                state->physicalNotifications.fetch_add(1, std::memory_order_relaxed);
                issued = true;
                try {
                    state->notifier();
                } catch (...) {
                    state->notifierExceptions.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                state->mergedNotifications.fetch_add(1, std::memory_order_relaxed);
            }
            return {.status = status, .notificationIssued = issued};
        }

        std::uint64_t generation() const noexcept { return generation_; }

    private:
        friend class SpscMailboxSource;
        ProducerHandle(std::shared_ptr<State> state, std::uint64_t generation)
            : state_(std::move(state)), generation_(generation) {}

        std::shared_ptr<State> state_;
        std::uint64_t generation_{};
    };

    using Notifier = std::function<void()>;
    using ClearHook = std::function<void(NotificationClearPhase)>;

    SpscMailboxSource(std::size_t capacity, Notifier notifier)
        : state_(std::make_shared<State>(capacity, std::move(notifier))) {}

    ~SpscMailboxSource() {
        if (!state_) return;
        state_->mailbox.beginStop();
        if (state_->activeProducerCalls.load(std::memory_order_acquire) == 0) {
            (void)state_->mailbox.cancelRemaining([](T&&) noexcept {});
        }
        state_->ownerAvailable.store(false, std::memory_order_release);
        state_->notificationPending.store(false, std::memory_order_release);
    }

    SpscMailboxSource(const SpscMailboxSource&) = delete;
    SpscMailboxSource& operator=(const SpscMailboxSource&) = delete;

    ProducerHandle producerHandle() const {
        return ProducerHandle(
            state_, state_->generation.load(std::memory_order_acquire));
    }

    template <typename Visitor>
    MailboxDrainResult drain(std::size_t maxItems, Visitor&& visitor) noexcept {
        if (!state_->ownerAvailable.load(std::memory_order_acquire)) {
            return {.status = MailboxDrainStatus::OwnerUnavailable};
        }
        if (drainActive_) {
            state_->reentrantDrainRejections.fetch_add(1, std::memory_order_relaxed);
            return {
                .status = MailboxDrainStatus::ReentrantDrainRejected,
                .remaining = state_->mailbox.depth(),
            };
        }
        struct DrainGuard {
            bool& active;
            explicit DrainGuard(bool& value) noexcept : active(value) { active = true; }
            ~DrainGuard() { active = false; }
        } guard(drainActive_);

        auto result = state_->mailbox.drain(maxItems, std::forward<Visitor>(visitor));
        if (result.status == MailboxDrainStatus::VisitorException) {
            state_->mailbox.beginStop();
        }
        if (result.remaining != 0) {
            result.needsContinuation = true;
            state_->ownerContinuations.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        invokeClearHook(NotificationClearPhase::BeforeClear);
        state_->notificationPending.store(false, std::memory_order_release);
        invokeClearHook(NotificationClearPhase::AfterClear);
        if (!state_->mailbox.empty()) {
            if (!state_->notificationPending.exchange(true, std::memory_order_acq_rel)) {
                state_->selfRearms.fetch_add(1, std::memory_order_relaxed);
                result.needsContinuation = true;
                state_->ownerContinuations.fetch_add(1, std::memory_order_relaxed);
            }
            result.remaining = state_->mailbox.depth();
        }
        return result;
    }

    void beginStop() noexcept { state_->mailbox.beginStop(); }

    template <typename Visitor>
    MailboxDrainResult cancelRemaining(Visitor&& visitor) noexcept {
        state_->mailbox.beginStop();
        auto result = state_->mailbox.cancelRemaining(std::forward<Visitor>(visitor));
        state_->notificationPending.store(false, std::memory_order_release);
        return result;
    }

    bool detach() noexcept {
        state_->mailbox.beginStop();
        if (state_->activeProducerCalls.load(std::memory_order_acquire) != 0 ||
            !state_->mailbox.empty()) {
            return false;
        }
        state_->notificationPending.store(false, std::memory_order_release);
        state_->ownerAvailable.store(false, std::memory_order_release);
        state_->generation.fetch_add(1, std::memory_order_acq_rel);
        state_->notifier = [] {};
        return true;
    }

    void setClearHook(ClearHook hook) { clearHook_ = std::move(hook); }

    MailboxSourceSnapshot snapshot() const noexcept {
        return {
            .mailbox = state_->mailbox.snapshot(),
            .generation = state_->generation.load(std::memory_order_acquire),
            .physicalNotifications =
                state_->physicalNotifications.load(std::memory_order_relaxed),
            .mergedNotifications =
                state_->mergedNotifications.load(std::memory_order_relaxed),
            .ownerContinuations =
                state_->ownerContinuations.load(std::memory_order_relaxed),
            .selfRearms = state_->selfRearms.load(std::memory_order_relaxed),
            .reentrantDrainRejections =
                state_->reentrantDrainRejections.load(std::memory_order_relaxed),
            .notifierExceptions =
                state_->notifierExceptions.load(std::memory_order_relaxed),
            .activeProducerCalls =
                state_->activeProducerCalls.load(std::memory_order_relaxed),
            .notificationPending =
                state_->notificationPending.load(std::memory_order_acquire),
            .ownerAvailable = state_->ownerAvailable.load(std::memory_order_acquire),
        };
    }

private:
    struct State {
        State(std::size_t capacity, Notifier notify)
            : mailbox(capacity), notifier(std::move(notify)),
              generation(nextGeneration.fetch_add(1, std::memory_order_relaxed)) {
            if (!notifier) throw std::invalid_argument("mailbox source requires notifier");
        }

        SpscMailbox<T> mailbox;
        Notifier notifier;
        std::atomic<std::uint64_t> generation;
        std::atomic<std::uint64_t> activeProducerCalls{};
        std::atomic<std::uint64_t> physicalNotifications{};
        std::atomic<std::uint64_t> mergedNotifications{};
        std::atomic<std::uint64_t> ownerContinuations{};
        std::atomic<std::uint64_t> selfRearms{};
        std::atomic<std::uint64_t> reentrantDrainRejections{};
        std::atomic<std::uint64_t> notifierExceptions{};
        std::atomic<bool> notificationPending{false};
        std::atomic<bool> ownerAvailable{true};
        inline static std::atomic<std::uint64_t> nextGeneration{1};
    };

    void invokeClearHook(NotificationClearPhase phase) noexcept {
        if (!clearHook_) return;
        try {
            clearHook_(phase);
        } catch (...) {
            state_->mailbox.beginStop();
        }
    }

    std::shared_ptr<State> state_;
    ClearHook clearHook_;
    bool drainActive_{false};
};

template <typename Packet>
struct DataPlaneCommand {
    std::uint64_t routeId{};
    std::uint64_t routeGeneration{};
    std::uint64_t enqueuedAtNs{};
    Packet packet;

    DataPlaneCommand(
        std::uint64_t id,
        std::uint64_t generation,
        std::uint64_t enqueued,
        Packet value)
        : routeId(id),
          routeGeneration(generation),
          enqueuedAtNs(enqueued),
          packet(std::move(value)) {}

    DataPlaneCommand(const DataPlaneCommand&) = delete;
    DataPlaneCommand& operator=(const DataPlaneCommand&) = delete;
    DataPlaneCommand(DataPlaneCommand&&) noexcept = default;
    DataPlaneCommand& operator=(DataPlaneCommand&&) noexcept = default;
};

}  // namespace gamenet::experimental::hp2
