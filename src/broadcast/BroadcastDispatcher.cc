#include "gamenet/broadcast/BroadcastDispatcher.h"

#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamenet::broadcast {
namespace {

std::size_t logicalBytes(std::size_t payloadBytes, std::size_t endpoints) noexcept {
    if (payloadBytes != 0 &&
        endpoints > (std::numeric_limits<std::size_t>::max)() / payloadBytes) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return payloadBytes * endpoints;
}

BroadcastReason postFailureReason(gamenet::net::PostResult result) noexcept {
    switch (result) {
    case gamenet::net::PostResult::QueueFull:
        return BroadcastReason::DispatchQueueFull;
    case gamenet::net::PostResult::Shutdown:
        return BroadcastReason::OwnerShutdown;
    case gamenet::net::PostResult::OwnerUnavailable:
        return BroadcastReason::OwnerUnavailable;
    case gamenet::net::PostResult::Accepted:
        return BroadcastReason::None;
    }
    return BroadcastReason::OwnerUnavailable;
}

BroadcastReason endpointFailureReason(
    gamenet::transport::EndpointResult result) noexcept {
    switch (result) {
    case gamenet::transport::EndpointResult::Accepted:
        return BroadcastReason::None;
    case gamenet::transport::EndpointResult::Closed:
        return BroadcastReason::EndpointClosed;
    case gamenet::transport::EndpointResult::OwnerUnavailable:
        return BroadcastReason::OwnerUnavailable;
    case gamenet::transport::EndpointResult::Overloaded:
        return BroadcastReason::EndpointOverloaded;
    case gamenet::transport::EndpointResult::WrongThread:
        return BroadcastReason::SendRejected;
    }
    return BroadcastReason::SendRejected;
}

void emitMetricNoexcept(
    const BroadcastMetricCallback& callback,
    const BroadcastMetric& metric) noexcept {
    if (!callback) return;
    try {
        callback(metric);
    } catch (...) {
        // Metrics are observational. They cannot change delivery or reservation
        // terminal state.
    }
}

}  // namespace

struct DispatchProgress::State {
    mutable std::mutex mutex;
    DispatchProgressSnapshot snapshot;
    bool sealed{false};
};

DispatchProgress::DispatchProgress(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DispatchProgressSnapshot DispatchProgress::snapshot() const noexcept {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}

struct BroadcastDispatcher::State {
    struct OwnerOutstanding {
        std::size_t tasks{};
        std::size_t bytes{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, OwnerOutstanding> owners;
    std::size_t tasks{};
    std::size_t bytes{};
    bool accepting{true};
};

BroadcastDispatcher::BroadcastDispatcher(
    DispatchLimits limits,
    BroadcastMetricCallback metricCallback)
    : limits_(limits),
      metricCallback_(std::move(metricCallback)),
      state_(std::make_shared<State>()) {
    if (limits_.maxEndpointsPerTask == 0 || limits_.maxBytesPerTask == 0 ||
        limits_.maxOutstandingTasksPerOwner == 0 ||
        limits_.maxOutstandingBytesPerOwner == 0 ||
        limits_.maxGlobalOutstandingBytes == 0 ||
        limits_.lowPriorityOutstandingBytes > limits_.maxGlobalOutstandingBytes) {
        throw std::invalid_argument("BroadcastDispatcher requires coherent positive limits");
    }
}

DispatchSummary BroadcastDispatcher::dispatch(BroadcastPlan plan) const {
    DispatchSummary summary;
    auto progressState = std::make_shared<DispatchProgress::State>();
    summary.progress = std::shared_ptr<DispatchProgress>(
        new DispatchProgress(progressState));

    const auto recordImmediateDrop =
        [this, &summary, &progressState, &plan](
            BroadcastReason reason,
            gamenet::transport::TransportSessionId id) {
            ++summary.droppedEndpoints;
            ++summary.reasonCounts[reasonIndex(reason)];
            {
                std::lock_guard lock(progressState->mutex);
                ++progressState->snapshot.droppedEndpoints;
                ++progressState->snapshot.reasonCounts[reasonIndex(reason)];
            }
            if (metricCallback_) {
                emitMetricNoexcept(metricCallback_, {
                    .event = BroadcastMetricEvent::Dropped,
                    .reason = reason,
                    .transportId = id,
                    .payloadBytes = plan.payload_ ? plan.payload_->size() : 0,
                });
            }
        };

    if (!plan.payload_) {
        recordImmediateDrop(BroadcastReason::InvalidPlan, {});
        std::lock_guard lock(progressState->mutex);
        progressState->sealed = true;
        progressState->snapshot.complete = true;
        return summary;
    }

    summary.droppedEndpoints = plan.dropped_;
    summary.reasonCounts = plan.reasonCounts_;
    {
        std::lock_guard lock(progressState->mutex);
        progressState->snapshot.droppedEndpoints = plan.dropped_;
        progressState->snapshot.reasonCounts = plan.reasonCounts_;
    }

    if (plan.payload_->size() > limits_.maxBytesPerTask) {
        for (const auto& batch : plan.batches_) {
            for (const auto& endpoint : batch.endpoints) {
                recordImmediateDrop(
                    BroadcastReason::DispatchTaskByteLimit,
                    endpoint ? endpoint->id()
                             : gamenet::transport::TransportSessionId{});
            }
        }
        std::lock_guard lock(progressState->mutex);
        progressState->sealed = true;
        progressState->snapshot.complete = true;
        return summary;
    }

    const auto byBytes = plan.payload_->empty()
        ? limits_.maxEndpointsPerTask
        : std::max<std::size_t>(1, limits_.maxBytesPerTask / plan.payload_->size());
    const auto chunkSize = std::min(limits_.maxEndpointsPerTask, byBytes);

    for (auto& batch : plan.batches_) {
        std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>>
            validEndpoints;
        validEndpoints.reserve(batch.endpoints.size());
        for (const auto& endpoint : batch.endpoints) {
            if (!endpoint) {
                recordImmediateDrop(BroadcastReason::InvalidPlan, {});
                continue;
            }
            const auto endpointExecutor = endpoint->ownerExecutor();
            if (endpointExecutor.id() != batch.ownerExecutor.id()) {
                recordImmediateDrop(BroadcastReason::InvalidPlan, endpoint->id());
                continue;
            }
            if (!endpoint->isOpen()) {
                recordImmediateDrop(BroadcastReason::EndpointClosed, endpoint->id());
                continue;
            }
            validEndpoints.push_back(endpoint);
        }

        for (std::size_t offset = 0; offset < validEndpoints.size();
             offset += chunkSize) {
            const auto end =
                std::min(validEndpoints.size(), offset + chunkSize);
            std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>>
                chunk(
                    validEndpoints.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    validEndpoints.begin() +
                        static_cast<std::ptrdiff_t>(end));
            const auto bytes = logicalBytes(plan.payload_->size(), chunk.size());
            const auto ownerId = batch.ownerExecutor.id();

            BroadcastReason reservationFailure = BroadcastReason::None;
            {
                std::lock_guard lock(state_->mutex);
                auto& owner = state_->owners[ownerId];
                if (!state_->accepting) {
                    reservationFailure = BroadcastReason::OwnerShutdown;
                } else if (owner.tasks >= limits_.maxOutstandingTasksPerOwner) {
                    reservationFailure =
                        BroadcastReason::OwnerOutstandingTaskLimit;
                } else if (
                    bytes > limits_.maxOutstandingBytesPerOwner - owner.bytes) {
                    reservationFailure =
                        BroadcastReason::OwnerOutstandingByteLimit;
                } else if (
                    bytes > limits_.maxGlobalOutstandingBytes - state_->bytes) {
                    reservationFailure =
                        BroadcastReason::GlobalOutstandingByteLimit;
                } else if (
                    plan.priority_ == BroadcastPriority::Low &&
                    bytes > limits_.lowPriorityOutstandingBytes - std::min(
                        state_->bytes, limits_.lowPriorityOutstandingBytes)) {
                    reservationFailure =
                        BroadcastReason::LowPrioritySoftLimit;
                } else {
                    ++owner.tasks;
                    owner.bytes += bytes;
                    ++state_->tasks;
                    state_->bytes += bytes;
                }
                if (reservationFailure != BroadcastReason::None &&
                    owner.tasks == 0 && owner.bytes == 0) {
                    state_->owners.erase(ownerId);
                }
            }
            if (reservationFailure != BroadcastReason::None) {
                for (const auto& endpoint : chunk) {
                    recordImmediateDrop(reservationFailure, endpoint->id());
                }
                continue;
            }

            {
                std::lock_guard lock(progressState->mutex);
                ++progressState->snapshot.outstandingTasks;
                progressState->snapshot.outstandingBytes += bytes;
            }

            auto payload = plan.payload_;
            auto metricCallback = metricCallback_;
            const auto dispatcherState = state_;
            const auto releaseReservation =
                [dispatcherState, progressState, ownerId, bytes] {
                    {
                        std::lock_guard lock(dispatcherState->mutex);
                        const auto found = dispatcherState->owners.find(ownerId);
                        if (found != dispatcherState->owners.end()) {
                            --found->second.tasks;
                            found->second.bytes -= bytes;
                            --dispatcherState->tasks;
                            dispatcherState->bytes -= bytes;
                            if (found->second.tasks == 0 &&
                                found->second.bytes == 0) {
                                dispatcherState->owners.erase(found);
                            }
                        }
                    }
                    std::lock_guard lock(progressState->mutex);
                    --progressState->snapshot.outstandingTasks;
                    progressState->snapshot.outstandingBytes -= bytes;
                    progressState->snapshot.complete =
                        progressState->sealed &&
                        progressState->snapshot.outstandingTasks == 0;
                };

            const auto posted = batch.ownerExecutor.post(
                [payload = std::move(payload),
                 endpoints = std::move(chunk),
                 metricCallback = std::move(metricCallback),
                 progressState,
                 releaseReservation]() mutable {
                    struct ReleaseGuard {
                        std::function<void()> release;
                        ~ReleaseGuard() { release(); }
                    } guard{releaseReservation};

                    for (const auto& endpoint : endpoints) {
                        BroadcastMetric metric{
                            .event = BroadcastMetricEvent::Scheduled,
                            .reason = BroadcastReason::None,
                            .transportId = endpoint->id(),
                            .payloadBytes = payload->size(),
                        };
                        emitMetricNoexcept(metricCallback, metric);
                        auto reason = BroadcastReason::None;
                        if (!endpoint->isOpen()) {
                            reason = BroadcastReason::EndpointClosed;
                        } else {
                            try {
                                reason = endpointFailureReason(endpoint->send(*payload));
                            } catch (...) {
                                reason = BroadcastReason::SendRejected;
                            }
                        }
                        metric.event = reason == BroadcastReason::None
                            ? BroadcastMetricEvent::Sent
                            : BroadcastMetricEvent::Dropped;
                        metric.reason = reason;
                        {
                            std::lock_guard lock(progressState->mutex);
                            if (reason == BroadcastReason::None) {
                                ++progressState->snapshot.acceptedEndpoints;
                            } else {
                                ++progressState->snapshot.droppedEndpoints;
                                ++progressState->snapshot.reasonCounts[
                                    reasonIndex(reason)];
                            }
                        }
                        emitMetricNoexcept(metricCallback, metric);
                    }
                });

            if (posted != gamenet::net::PostResult::Accepted) {
                releaseReservation();
                const auto reason = postFailureReason(posted);
                for (std::size_t index = offset; index < end; ++index) {
                    recordImmediateDrop(reason, validEndpoints[index]->id());
                }
                continue;
            }

            const auto admitted = end - offset;
            summary.scheduledEndpoints += admitted;
            summary.acceptedEndpoints += admitted;
            ++summary.scheduledTasks;
            {
                std::lock_guard lock(progressState->mutex);
                progressState->snapshot.scheduledEndpoints += admitted;
            }
        }
    }

    {
        std::lock_guard lock(progressState->mutex);
        progressState->sealed = true;
        progressState->snapshot.complete =
            progressState->snapshot.outstandingTasks == 0;
    }
    return summary;
}

DispatchOutstandingSnapshot BroadcastDispatcher::outstanding() const noexcept {
    std::lock_guard lock(state_->mutex);
    return {
        .tasks = state_->tasks,
        .bytes = state_->bytes,
    };
}

void BroadcastDispatcher::shutdown() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->accepting = false;
}

bool BroadcastDispatcher::accepting() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->accepting;
}

}  // namespace gamenet::broadcast
