// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp5/CreditLease.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::hp5 {

SharedCreditBudget::SharedCreditBudget(std::size_t hardLimitBytes)
    : hardLimitBytes_(hardLimitBytes) {
    if (hardLimitBytes == 0) {
        throw std::invalid_argument("shared credit hard limit must be positive");
    }
}

bool SharedCreditBudget::tryReserve(std::size_t bytes) noexcept {
    if (bytes == 0) return true;
    std::size_t current = reservedBytes_.load(std::memory_order_acquire);
    for (;;) {
        if (current > hardLimitBytes_ || bytes > hardLimitBytes_ - current) {
            rejectedReservations_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto candidate = current + bytes;
        if (reservedBytes_.compare_exchange_weak(
                current, candidate,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            modeledAtomicMutations_.fetch_add(1, std::memory_order_relaxed);
            updatePeak(candidate);
            return true;
        }
    }
}

void SharedCreditBudget::release(std::size_t bytes) noexcept {
    if (bytes == 0) return;
    const auto previous = reservedBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) std::terminate();
    modeledAtomicMutations_.fetch_add(1, std::memory_order_relaxed);
}

SharedCreditSnapshot SharedCreditBudget::snapshot() const noexcept {
    return SharedCreditSnapshot{
        .reservedBytes = reservedBytes_.load(std::memory_order_acquire),
        .peakReservedBytes = peakReservedBytes_.load(std::memory_order_relaxed),
        .rejectedReservations = rejectedReservations_.load(std::memory_order_relaxed),
        .modeledAtomicMutations = modeledAtomicMutations_.load(std::memory_order_relaxed),
    };
}

std::size_t SharedCreditBudget::hardLimitBytes() const noexcept {
    return hardLimitBytes_;
}

void SharedCreditBudget::updatePeak(std::size_t candidate) noexcept {
    auto peak = peakReservedBytes_.load(std::memory_order_relaxed);
    while (peak < candidate &&
           !peakReservedBytes_.compare_exchange_weak(
               peak, candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void LoopCreditLeaseOptions::validate() const {
    if (maxConnections == 0 || loopHardLimitBytes == 0 ||
        leaseQuantumBytes == 0 || leaseQuantumBytes > loopHardLimitBytes ||
        retainedIdleCreditBytes > loopHardLimitBytes) {
        throw std::invalid_argument("invalid loop credit lease options");
    }
}

LoopCreditLease::LoopCreditLease(
    LoopCreditLeaseOptions options,
    std::shared_ptr<SharedCreditBudget> serverBudget,
    std::shared_ptr<SharedCreditBudget> globalBudget)
    : options_(options),
      serverBudget_(std::move(serverBudget)),
      globalBudget_(std::move(globalBudget)),
      owner_(std::this_thread::get_id()) {
    options_.validate();
    if (!serverBudget_ || !globalBudget_) {
        throw std::invalid_argument("credit lease requires server and global budgets");
    }
    slots_.resize(options_.maxConnections);
}

LoopCreditLease::~LoopCreditLease() {
    // Emergency RAII rollback. Correct callers prove the explicit settled state.
    if (leasedBytes_ != 0) returnCredit(leasedBytes_);
}

ConnectionRegistration LoopCreditLease::registerConnection(
    std::size_t hardLimitBytes) {
    if (!isOwner()) return {.status = CreditStatus::WrongOwner};
    if (stopped_) return {.status = CreditStatus::Stopped};
    if (hardLimitBytes == 0 || hardLimitBytes > options_.loopHardLimitBytes) {
        return {.status = CreditStatus::Invalid};
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.active) continue;
        slot.active = true;
        slot.hardLimitBytes = hardLimitBytes;
        slot.pendingBytes = 0;
        ++activeConnections_;
        return {
            .status = CreditStatus::Accepted,
            .handle = ConnectionCreditHandle{
                .index = static_cast<std::uint32_t>(index),
                .generation = slot.generation,
            },
        };
    }
    return {.status = CreditStatus::NoConnectionSlot};
}

CreditStatus LoopCreditLease::retireConnection(ConnectionCreditHandle handle) {
    if (!isOwner()) return CreditStatus::WrongOwner;
    auto* slot = resolve(handle);
    if (slot == nullptr) return CreditStatus::StaleHandle;
    if (slot->pendingBytes != 0) return CreditStatus::PendingBytes;
    slot->active = false;
    slot->hardLimitBytes = 0;
    slot->generation = nextGeneration(slot->generation);
    --activeConnections_;
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::tryReserve(
    ConnectionCreditHandle handle,
    std::size_t bytes) {
    if (!isOwner()) return CreditStatus::WrongOwner;
    if (stopped_) return CreditStatus::Stopped;
    auto* slot = resolve(handle);
    if (slot == nullptr) return CreditStatus::StaleHandle;
    if (bytes == 0) return CreditStatus::Accepted;
    if (slot->pendingBytes > slot->hardLimitBytes ||
        bytes > slot->hardLimitBytes - slot->pendingBytes) {
        return CreditStatus::ConnectionLimit;
    }
    if (usedBytes_ > options_.loopHardLimitBytes ||
        bytes > options_.loopHardLimitBytes - usedBytes_) {
        return CreditStatus::LoopLimit;
    }
    const auto available = leasedBytes_ - usedBytes_;
    if (bytes > available) {
        const auto status = refill(bytes - available);
        if (status != CreditStatus::Accepted) return status;
    }
    slot->pendingBytes += bytes;
    usedBytes_ += bytes;
    acceptedBytes_ += bytes;
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::release(
    ConnectionCreditHandle handle,
    std::size_t bytes) {
    if (!isOwner()) return CreditStatus::WrongOwner;
    auto* slot = resolve(handle);
    if (slot == nullptr) return CreditStatus::StaleHandle;
    if (bytes > slot->pendingBytes) return CreditStatus::Invalid;
    slot->pendingBytes -= bytes;
    usedBytes_ -= bytes;
    releasedBytes_ += bytes;
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::cancel(ConnectionCreditHandle handle) {
    if (!isOwner()) return CreditStatus::WrongOwner;
    auto* slot = resolve(handle);
    if (slot == nullptr) return CreditStatus::StaleHandle;
    discardedBytes_ += slot->pendingBytes;
    usedBytes_ -= slot->pendingBytes;
    slot->pendingBytes = 0;
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::trimIdleCredit() {
    if (!isOwner()) return CreditStatus::WrongOwner;
    const auto keep = (std::max)(usedBytes_, options_.retainedIdleCreditBytes);
    if (leasedBytes_ > keep) returnCredit(leasedBytes_ - keep);
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::beginStop() {
    if (!isOwner()) return CreditStatus::WrongOwner;
    stopped_ = true;
    return CreditStatus::Accepted;
}

CreditStatus LoopCreditLease::cancelAll() {
    if (!isOwner()) return CreditStatus::WrongOwner;
    if (!stopped_) return CreditStatus::Invalid;
    for (auto& slot : slots_) {
        if (!slot.active) continue;
        discardedBytes_ += slot.pendingBytes;
        usedBytes_ -= slot.pendingBytes;
        slot.pendingBytes = 0;
        slot.active = false;
        slot.hardLimitBytes = 0;
        slot.generation = nextGeneration(slot.generation);
    }
    activeConnections_ = 0;
    if (leasedBytes_ != 0) returnCredit(leasedBytes_);
    return CreditStatus::Accepted;
}

bool LoopCreditLease::tryShutdown() const noexcept {
    return isOwner() && snapshot().settled();
}

LoopCreditLeaseSnapshot LoopCreditLease::snapshot() const noexcept {
    if (!isOwner()) return {};
    return LoopCreditLeaseSnapshot{
        .activeConnections = activeConnections_,
        .usedBytes = usedBytes_,
        .leasedBytes = leasedBytes_,
        .acceptedBytes = acceptedBytes_,
        .releasedBytes = releasedBytes_,
        .discardedBytes = discardedBytes_,
        .refillCount = refillCount_,
        .returnCount = returnCount_,
        .stopped = stopped_,
    };
}

bool LoopCreditLease::isOwner() const noexcept {
    return owner_ == std::this_thread::get_id();
}

LoopCreditLease::ConnectionSlot* LoopCreditLease::resolve(
    ConnectionCreditHandle handle) noexcept {
    if (!handle.valid() || handle.index >= slots_.size()) return nullptr;
    auto& slot = slots_[handle.index];
    if (!slot.active || slot.generation != handle.generation) return nullptr;
    return &slot;
}

CreditStatus LoopCreditLease::refill(std::size_t bytes) {
    const auto headroom = options_.loopHardLimitBytes - leasedBytes_;
    const auto preferred = (std::max)(bytes, options_.leaseQuantumBytes);
    const auto grant = (std::min)(preferred, headroom);
    if (grant < bytes) return CreditStatus::LoopLimit;
    if (!serverBudget_->tryReserve(grant)) return CreditStatus::ServerLimit;
    if (!globalBudget_->tryReserve(grant)) {
        serverBudget_->release(grant);
        return CreditStatus::GlobalLimit;
    }
    leasedBytes_ += grant;
    ++refillCount_;
    return CreditStatus::Accepted;
}

void LoopCreditLease::returnCredit(std::size_t bytes) noexcept {
    if (bytes == 0) return;
    globalBudget_->release(bytes);
    serverBudget_->release(bytes);
    leasedBytes_ -= bytes;
    ++returnCount_;
}

std::uint32_t LoopCreditLease::nextGeneration(
    std::uint32_t generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

}  // namespace gamenet::experimental::hp5
