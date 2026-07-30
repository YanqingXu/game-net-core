#include "gamenet/core/net/TcpOutputMemoryBudget.h"

#include "gamenet/core/base/Logger.h"

#include <stdexcept>

namespace gamenet::net {

void TcpOutputMemoryBudgetOptions::validate() const {
    if (hardLimitBytes == 0) {
        throw std::invalid_argument(
            "TCP output-memory hard limit must be greater than zero");
    }
    if (recoveryThresholdBytes >= hardLimitBytes) {
        throw std::invalid_argument(
            "TCP output-memory recovery threshold must be below the hard limit");
    }
}

TcpOutputMemoryBudget::TcpOutputMemoryBudget(
    TcpOutputMemoryBudgetOptions options)
    : options_(options) {
    options_.validate();
}

TcpOutputMemoryBudgetSnapshot
TcpOutputMemoryBudget::snapshot() const noexcept {
    return TcpOutputMemoryBudgetSnapshot{
        .pendingBytes =
            pendingBytes_.load(std::memory_order_acquire),
        .peakPendingBytes =
            peakPendingBytes_.load(std::memory_order_relaxed),
        .rejectedReservations =
            rejectedReservations_.load(std::memory_order_relaxed),
        .overloaded =
            overloaded_.load(std::memory_order_acquire),
    };
}

std::size_t TcpOutputMemoryBudget::hardLimitBytes() const noexcept {
    return options_.hardLimitBytes;
}

std::size_t
TcpOutputMemoryBudget::recoveryThresholdBytes() const noexcept {
    return options_.recoveryThresholdBytes;
}

bool TcpOutputMemoryBudget::tryReserve(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }

    std::size_t current =
        pendingBytes_.load(std::memory_order_acquire);
    for (;;) {
        if (overloaded_.load(std::memory_order_acquire)) {
            if (current > options_.recoveryThresholdBytes) {
                rejectedReservations_.fetch_add(
                    1, std::memory_order_relaxed);
                return false;
            }

            bool expected = true;
            (void)overloaded_.compare_exchange_strong(
                expected,
                false,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
            current = pendingBytes_.load(std::memory_order_acquire);
            continue;
        }

        if (current > options_.hardLimitBytes ||
            bytes > options_.hardLimitBytes - current) {
            // A single oversized request against an otherwise recovered
            // budget must not latch out unrelated smaller sends.
            if (current > options_.recoveryThresholdBytes) {
                overloaded_.store(true, std::memory_order_release);
            }
            rejectedReservations_.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }

        const std::size_t candidate = current + bytes;
        if (pendingBytes_.compare_exchange_weak(
                current,
                candidate,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            updatePeak(candidate);
            return true;
        }
    }
}

void TcpOutputMemoryBudget::release(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }

    const std::size_t previous =
        pendingBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) {
        LOG_FATAL << "TCP output-memory reservation underflow";
    }

    const std::size_t remaining = previous - bytes;
    if (remaining <= options_.recoveryThresholdBytes) {
        overloaded_.store(false, std::memory_order_release);
    }
}

void TcpOutputMemoryBudget::updatePeak(
    std::size_t candidate) noexcept {
    std::size_t peak =
        peakPendingBytes_.load(std::memory_order_relaxed);
    while (peak < candidate &&
           !peakPendingBytes_.compare_exchange_weak(
               peak,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

}  // namespace gamenet::net
