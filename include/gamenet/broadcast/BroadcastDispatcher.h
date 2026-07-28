#pragma once

#include "gamenet/broadcast/BroadcastTypes.h"

#include <array>
#include <cstddef>
#include <memory>

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
};

class BroadcastDispatcher {
public:
    explicit BroadcastDispatcher(
        DispatchLimits limits = {},
        BroadcastMetricCallback metricCallback = {});

    DispatchSummary dispatch(BroadcastPlan plan) const;
    DispatchOutstandingSnapshot outstanding() const noexcept;
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
