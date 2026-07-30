#pragma once

#include "gamenet/broadcast/BroadcastTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gamenet::broadcast {

struct DispatchLimits {
    std::size_t maxEndpointsPerTask{64};
    std::size_t maxBytesPerTask{256U * 1024U};
    std::size_t maxOutstandingTasksPerOwner{1024};
    std::size_t maxOutstandingBytesPerOwner{64U * 1024U * 1024U};
    std::size_t maxGlobalOutstandingBytes{256U * 1024U * 1024U};
    std::size_t lowPriorityOutstandingBytes{128U * 1024U * 1024U};
};

struct DispatchProgressSnapshot {
    std::size_t scheduledEndpoints{};
    std::size_t acceptedEndpoints{};
    std::size_t droppedEndpoints{};
    std::size_t outstandingTasks{};
    std::size_t outstandingBytes{};
    bool complete{};
    std::array<std::size_t, kBroadcastReasonCount> reasonCounts{};

    std::size_t reasonCount(BroadcastReason reason) const noexcept {
        const auto index = reasonIndex(reason);
        return index < reasonCounts.size() ? reasonCounts[index] : 0;
    }
};

class DispatchProgress {
public:
    DispatchProgressSnapshot snapshot() const noexcept;

private:
    struct State;
    explicit DispatchProgress(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;

    friend class BroadcastDispatcher;
};

struct DispatchSummary {
    std::size_t scheduledEndpoints{};
    std::size_t scheduledTasks{};
    std::size_t acceptedEndpoints{};
    std::size_t droppedEndpoints{};
    std::array<std::size_t, kBroadcastReasonCount> reasonCounts{};
    std::shared_ptr<DispatchProgress> progress;

    std::size_t reasonCount(BroadcastReason reason) const noexcept {
        const auto index = reasonIndex(reason);
        return index < reasonCounts.size() ? reasonCounts[index] : 0;
    }
};

struct DispatchOutstandingSnapshot {
    std::size_t tasks{};
    std::size_t bytes{};
    std::size_t peakTasks{};
    std::size_t peakBytes{};
    std::uint64_t rejectedGlobalByteReservations{};
    bool accepting{};
};

struct DispatchOwnerOutstandingSnapshot {
    std::uint64_t ownerId{};
    std::size_t tasks{};
    std::size_t bytes{};
    std::size_t peakTasks{};
    std::size_t peakBytes{};
    std::uint64_t rejectedTaskReservations{};
    std::uint64_t rejectedByteReservations{};
};

struct DispatchOutstandingStats {
    DispatchOutstandingSnapshot global;
    std::vector<DispatchOwnerOutstandingSnapshot> owners;
};

class BroadcastDispatcher {
public:
    explicit BroadcastDispatcher(
        DispatchLimits limits = {},
        BroadcastMetricCallback metricCallback = {});

    DispatchSummary dispatch(BroadcastPlan plan) const;
    // Cross-thread-safe atomic snapshot. Current fields converge exactly and
    // tasks==0 observes prior scope releases; other multi-counter combinations
    // are diagnostic rather than transactional.
    DispatchOutstandingSnapshot outstanding() const noexcept;
    // Low-frequency per-owner diagnostic snapshot.
    DispatchOutstandingStats outstandingStats() const;
    // Thread-safe, idempotent admission close. Already-admitted tasks retain
    // their payload/endpoints and publish terminal progress.
    void shutdown() noexcept;
    bool accepting() const noexcept;

private:
    struct State;

    DispatchLimits limits_;
    BroadcastMetricCallback metricCallback_;
    std::shared_ptr<State> state_;
};

}  // namespace gamenet::broadcast
