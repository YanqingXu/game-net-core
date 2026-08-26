// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp3/EpollSlotDispatch.h"

#include <limits>
#include <stdexcept>

namespace gamenet::experimental::hp3 {

namespace {

constexpr std::uint32_t kAllEvents =
    kReadEvent | kWriteEvent | kErrorEvent | kCloseEvent;
constexpr std::uint32_t kAlwaysDelivered = kErrorEvent | kCloseEvent;

}  // namespace

std::uint64_t SlotIdentity::token() const noexcept {
    if (!valid()) return 0;
    return (static_cast<std::uint64_t>(generation) << 32U) |
        (static_cast<std::uint64_t>(slotIndex) + 1U);
}

bool SlotIdentity::valid() const noexcept {
    return source >= 0 && generation != 0 &&
        slotIndex != (std::numeric_limits<std::uint32_t>::max)();
}

EpollSlotDispatchPrototype::EpollSlotDispatchPrototype(SlotDispatchOptions options)
    : ownerThread_(std::this_thread::get_id()), options_(options) {
    if (options_.capacity == 0 || options_.maxNoticesPerBatch == 0 ||
        options_.capacity >
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)() - 1U) ||
        options_.maxNoticesPerBatch > options_.capacity) {
        throw std::invalid_argument(
            "slot dispatch requires finite capacity and notice bounds");
    }
    slots_.resize(options_.capacity);
    freeSlots_.reserve(options_.capacity);
    notices_.reserve(options_.maxNoticesPerBatch);
    sourceToSlot_.reserve(options_.capacity);
    for (std::size_t index = options_.capacity; index != 0; --index) {
        freeSlots_.push_back(static_cast<std::uint32_t>(index - 1));
    }
}

SlotRegistrationResult EpollSlotDispatchPrototype::registerOrUpdate(
    int source,
    void* target,
    std::uint32_t interests) {
    assertOwnerThread();
    if (!accepting_) return {.result = SlotDispatchResult::Stopped};
    if (source < 0 || target == nullptr || !validInterests(interests)) {
        return {.result = SlotDispatchResult::Invalid};
    }

    const auto existing = sourceToSlot_.find(source);
    if (existing != sourceToSlot_.end()) {
        Slot& slot = slots_[existing->second];
        const SlotIdentity identity{source, existing->second, slot.generation};
        if (!slot.active || slot.target != target) {
            return {.result = SlotDispatchResult::Conflict, .identity = identity};
        }
        slot.interests = interests;
        return {.result = SlotDispatchResult::Accepted, .identity = identity};
    }

    if (interests == 0) return {.result = SlotDispatchResult::Invalid};
    if (freeSlots_.empty()) return {.result = SlotDispatchResult::Capacity};

    const auto index = freeSlots_.back();
    freeSlots_.pop_back();
    Slot& slot = slots_[index];
    if (slot.generation == (std::numeric_limits<std::uint32_t>::max)()) {
        freeSlots_.push_back(index);
        throw std::overflow_error("slot dispatch generation exhausted");
    }
    ++slot.generation;
    if (slot.generation == 0) {
        freeSlots_.push_back(index);
        throw std::overflow_error("slot dispatch generation wrapped");
    }
    slot.source = source;
    slot.target = target;
    slot.interests = interests;
    slot.lastBatchEpoch = 0;
    slot.noticeIndex = 0;
    slot.active = true;
    try {
        sourceToSlot_.emplace(source, index);
    } catch (...) {
        slot.source = -1;
        slot.target = nullptr;
        slot.interests = 0;
        slot.active = false;
        freeSlots_.push_back(index);
        throw;
    }
    return {
        .result = SlotDispatchResult::Accepted,
        .identity = SlotIdentity{source, index, slot.generation},
    };
}

SlotDispatchResult EpollSlotDispatchPrototype::cancel(
    SlotIdentity identity,
    void* target) {
    assertOwnerThread();
    if (!identity.valid() || target == nullptr ||
        identity.slotIndex >= slots_.size()) {
        return SlotDispatchResult::Invalid;
    }
    const auto existing = sourceToSlot_.find(identity.source);
    if (existing == sourceToSlot_.end()) {
        return SlotDispatchResult::NotRegistered;
    }
    if (existing->second != identity.slotIndex) {
        return SlotDispatchResult::Conflict;
    }
    Slot& slot = slots_[identity.slotIndex];
    if (!slot.active || slot.generation != identity.generation ||
        slot.source != identity.source || slot.target != target) {
        return SlotDispatchResult::Conflict;
    }

    sourceToSlot_.erase(existing);
    slot.source = -1;
    slot.target = nullptr;
    slot.interests = 0;
    slot.lastBatchEpoch = 0;
    slot.noticeIndex = 0;
    slot.active = false;
    freeSlots_.push_back(identity.slotIndex);
    return SlotDispatchResult::Accepted;
}

SlotDecodeBatch EpollSlotDispatchPrototype::decode(
    std::span<const NativeReadinessEvent> nativeEvents) {
    assertOwnerThread();
    SlotDecodeMetrics metrics;
    const bool epochWillReset =
        batchEpoch_ == (std::numeric_limits<std::uint32_t>::max)();
    metrics.batchEpoch = beginBatch();
    metrics.epochResets = epochWillReset ? 1U : 0U;
    metrics.nativeEvents = nativeEvents.size();

    for (const auto& nativeEvent : nativeEvents) {
        if (nativeEvent.token == kWakeupToken) {
            ++metrics.wakeupEvents;
            continue;
        }
        const auto identity = unpack(nativeEvent.token);
        if (identity.generation == 0 || identity.slotIndex >= slots_.size()) {
            ++metrics.malformedEvents;
            continue;
        }

        ++metrics.slotProbes;
        Slot& slot = slots_[identity.slotIndex];
        if (!slot.active || slot.generation != identity.generation) {
            ++metrics.staleEvents;
            continue;
        }

        const auto events = (nativeEvent.events & kAllEvents) &
            (slot.interests | kAlwaysDelivered);
        if (events == 0) {
            ++metrics.filteredEvents;
            continue;
        }

        if (slot.lastBatchEpoch == batchEpoch_) {
            if (slot.noticeIndex >= notices_.size() ||
                notices_[slot.noticeIndex].identity.slotIndex != identity.slotIndex ||
                notices_[slot.noticeIndex].identity.generation != identity.generation) {
                throw std::logic_error("slot dispatch batch merge identity corrupted");
            }
            notices_[slot.noticeIndex].events |= events;
            ++metrics.mergedEvents;
            continue;
        }
        if (notices_.size() == options_.maxNoticesPerBatch) {
            ++metrics.budgetDroppedEvents;
            metrics.budgetExhausted = true;
            continue;
        }

        slot.lastBatchEpoch = batchEpoch_;
        slot.noticeIndex = notices_.size();
        notices_.push_back({
            .identity = SlotIdentity{slot.source, identity.slotIndex, slot.generation},
            .target = slot.target,
            .events = events,
        });
    }
    metrics.deliveredNotices = notices_.size();
    // These two counters are structural assertions for the candidate path.
    metrics.waitHashLookups = 0;
    metrics.mergeProbes = 0;
    return {.notices = notices_, .metrics = metrics};
}

void EpollSlotDispatchPrototype::finishBatch() {
    assertOwnerThread();
    notices_.clear();
}

bool EpollSlotDispatchPrototype::isCurrent(
    SlotIdentity identity,
    const void* target) const {
    assertOwnerThread();
    if (!identity.valid() || target == nullptr || identity.slotIndex >= slots_.size()) {
        return false;
    }
    const Slot& slot = slots_[identity.slotIndex];
    return slot.active && slot.source == identity.source &&
        slot.generation == identity.generation && slot.target == target;
}

void EpollSlotDispatchPrototype::beginStop() {
    assertOwnerThread();
    accepting_ = false;
}

bool EpollSlotDispatchPrototype::tryShutdown() {
    assertOwnerThread();
    if (accepting_ || !sourceToSlot_.empty() || !notices_.empty()) return false;
    shutdown_ = true;
    return true;
}

SlotArenaSnapshot EpollSlotDispatchPrototype::snapshot() const {
    assertOwnerThread();
    return {
        .capacity = slots_.size(),
        .registrations = sourceToSlot_.size(),
        .freeSlots = freeSlots_.size(),
        .pendingNotices = notices_.size(),
        .accepting = accepting_,
        .shutdown = shutdown_,
    };
}

void EpollSlotDispatchPrototype::forceBatchEpochForTesting(std::uint32_t epoch) {
    assertOwnerThread();
    batchEpoch_ = epoch;
}

void EpollSlotDispatchPrototype::assertOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("slot dispatch mutation requires owner thread");
    }
}

std::uint32_t EpollSlotDispatchPrototype::beginBatch() {
    notices_.clear();
    if (batchEpoch_ == (std::numeric_limits<std::uint32_t>::max)()) {
        for (auto& slot : slots_) slot.lastBatchEpoch = 0;
        batchEpoch_ = 1;
        return batchEpoch_;
    }
    ++batchEpoch_;
    if (batchEpoch_ == 0) batchEpoch_ = 1;
    return batchEpoch_;
}

bool EpollSlotDispatchPrototype::validInterests(std::uint32_t interests) noexcept {
    return (interests & ~(kReadEvent | kWriteEvent)) == 0;
}

SlotIdentity EpollSlotDispatchPrototype::unpack(std::uint64_t token) noexcept {
    const auto low = static_cast<std::uint32_t>(token);
    const auto generation = static_cast<std::uint32_t>(token >> 32U);
    if (low == 0) {
        return {.source = -1, .slotIndex = (std::numeric_limits<std::uint32_t>::max)(),
                .generation = generation};
    }
    return {.source = -1, .slotIndex = low - 1U, .generation = generation};
}

}  // namespace gamenet::experimental::hp3
