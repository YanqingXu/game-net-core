#include "gamenet/core/net/NetworkMemoryRetention.h"

#include "detail/NetworkMemoryRetentionTracker.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>

namespace gamenet::net {

namespace {

using detail::NetworkFixedStorageCategory;

constexpr std::size_t kCategoryCount = 3;

constexpr std::size_t categoryIndex(
    NetworkFixedStorageCategory category) noexcept {
    switch (category) {
    case NetworkFixedStorageCategory::AcceptExFixedPool:
        return 0;
    case NetworkFixedStorageCategory::IocpCompletionBatch:
        return 1;
    case NetworkFixedStorageCategory::ConnectionLocalRead:
        return 2;
    }
    std::abort();
}

std::array<std::atomic<std::size_t>, kCategoryCount> currentBytes{};
std::array<std::atomic<std::size_t>, kCategoryCount> peakBytes{};
std::atomic<std::size_t> totalBytes{0};
std::atomic<std::size_t> peakTotalBytes{0};

void updatePeak(
    std::atomic<std::size_t>& peak,
    std::size_t value) noexcept {
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (value > observed &&
           !peak.compare_exchange_weak(
               observed,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

std::size_t loadCurrent(
    NetworkFixedStorageCategory category) noexcept {
    return currentBytes[categoryIndex(category)].load(
        std::memory_order_relaxed);
}

std::size_t loadPeak(
    NetworkFixedStorageCategory category) noexcept {
    return peakBytes[categoryIndex(category)].load(
        std::memory_order_relaxed);
}

}  // namespace

namespace detail {

void retainNetworkFixedStorage(
    NetworkFixedStorageCategory category,
    std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::size_t index = categoryIndex(category);
    const std::size_t categoryCurrent =
        currentBytes[index].fetch_add(bytes, std::memory_order_relaxed) +
        bytes;
    updatePeak(peakBytes[index], categoryCurrent);

    const std::size_t totalCurrent =
        totalBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    updatePeak(peakTotalBytes, totalCurrent);
}

void releaseNetworkFixedStorage(
    NetworkFixedStorageCategory category,
    std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::size_t categoryPrevious =
        currentBytes[categoryIndex(category)].fetch_sub(
            bytes,
            std::memory_order_relaxed);
    const std::size_t totalPrevious =
        totalBytes.fetch_sub(bytes, std::memory_order_relaxed);
    if (categoryPrevious < bytes || totalPrevious < bytes) {
        std::abort();
    }
}

}  // namespace detail

NetworkFixedStorageRetentionSnapshot
networkFixedStorageRetentionSnapshot() noexcept {
    NetworkFixedStorageRetentionSnapshot snapshot;
    snapshot.acceptExFixedPoolBytes =
        loadCurrent(NetworkFixedStorageCategory::AcceptExFixedPool);
    snapshot.peakAcceptExFixedPoolBytes =
        loadPeak(NetworkFixedStorageCategory::AcceptExFixedPool);
    snapshot.iocpCompletionBatchBytes =
        loadCurrent(NetworkFixedStorageCategory::IocpCompletionBatch);
    snapshot.peakIocpCompletionBatchBytes =
        loadPeak(NetworkFixedStorageCategory::IocpCompletionBatch);
    snapshot.connectionLocalReadBytes =
        loadCurrent(NetworkFixedStorageCategory::ConnectionLocalRead);
    snapshot.peakConnectionLocalReadBytes =
        loadPeak(NetworkFixedStorageCategory::ConnectionLocalRead);
    snapshot.totalRetainedBytes =
        totalBytes.load(std::memory_order_relaxed);
    snapshot.peakTotalRetainedBytes =
        peakTotalBytes.load(std::memory_order_relaxed);
#ifdef _WIN32
    snapshot.acceptExSlotLimitPerAcceptor = 64;
    snapshot.iocpCompletionBatchEntriesPerLoop = 64;
    snapshot.connectionLocalReadChunkLimitBytes = 4U * 1024U;
#endif
    return snapshot;
}

}  // namespace gamenet::net
