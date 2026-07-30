#include "gamenet/broadcast/BroadcastDispatcher.h"

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
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

bool fitsWithin(
    std::size_t current,
    std::size_t amount,
    std::size_t limit) noexcept {
    return current <= limit && amount <= limit - current;
}

class ReservationRollback {
public:
    explicit ReservationRollback(
        const std::function<void()>& release) noexcept
        : release_(&release) {}

    ~ReservationRollback() {
        if (armed_) {
            (*release_)();
        }
    }

    void dismiss() noexcept { armed_ = false; }

private:
    const std::function<void()>* release_;
    bool armed_{true};
};

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
    class AtomicLimit {
    public:
        struct ReserveResult {
            bool accepted{false};
            std::size_t observedCurrent{0};
        };

        ReserveResult tryReserve(
            std::size_t amount,
            std::size_t limit) noexcept {
            if (amount == 0) {
                return {
                    .accepted = true,
                    .observedCurrent =
                        current_.load(std::memory_order_acquire),
                };
            }

            std::size_t current =
                current_.load(std::memory_order_acquire);
            for (;;) {
                if (!fitsWithin(current, amount, limit)) {
                    rejected_.fetch_add(1, std::memory_order_relaxed);
                    return {
                        .accepted = false,
                        .observedCurrent = current,
                    };
                }
                const std::size_t candidate = current + amount;
                if (current_.compare_exchange_weak(
                        current,
                        candidate,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    updatePeak(candidate);
                    return {
                        .accepted = true,
                        .observedCurrent = candidate,
                    };
                }
            }
        }

        void release(std::size_t amount) noexcept {
            if (amount == 0) {
                return;
            }
            const std::size_t previous =
                current_.fetch_sub(amount, std::memory_order_acq_rel);
            if (previous < amount) {
                LOG_FATAL << "Broadcast outstanding reservation underflow";
            }
        }

        std::size_t current() const noexcept {
            return current_.load(std::memory_order_acquire);
        }

        std::size_t peak() const noexcept {
            return peak_.load(std::memory_order_relaxed);
        }

        std::uint64_t rejected() const noexcept {
            return rejected_.load(std::memory_order_relaxed);
        }

    private:
        void updatePeak(std::size_t candidate) noexcept {
            std::size_t peak =
                peak_.load(std::memory_order_relaxed);
            while (peak < candidate &&
                   !peak_.compare_exchange_weak(
                       peak,
                       candidate,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }

        std::atomic<std::size_t> current_{0};
        std::atomic<std::size_t> peak_{0};
        std::atomic<std::uint64_t> rejected_{0};
    };

    struct OwnerOutstanding {
        OwnerOutstanding(
            std::uint64_t owner,
            const DispatchLimits& limits) noexcept
            : ownerId(owner),
              taskLimit(limits.maxOutstandingTasksPerOwner),
              byteLimit(limits.maxOutstandingBytesPerOwner) {}

        std::uint64_t ownerId{};
        std::size_t taskLimit{};
        std::size_t byteLimit{};
        AtomicLimit tasks;
        AtomicLimit bytes;
    };

    using OwnerRegistry =
        std::vector<std::shared_ptr<OwnerOutstanding>>;

    explicit State(const DispatchLimits& limits)
        : globalByteLimit(limits.maxGlobalOutstandingBytes),
          lowPriorityByteLimit(limits.lowPriorityOutstandingBytes),
          owners(std::make_shared<const OwnerRegistry>()) {}

    std::shared_ptr<OwnerOutstanding> ownerFor(
        std::uint64_t ownerId,
        const DispatchLimits& limits) {
        auto registry = owners.load(std::memory_order_acquire);
        if (const auto found = findOwner(*registry, ownerId)) {
            return found;
        }

        std::lock_guard lock(ownerRegistrationMutex);
        registry = owners.load(std::memory_order_acquire);
        if (const auto found = findOwner(*registry, ownerId)) {
            return found;
        }

        auto owner =
            std::make_shared<OwnerOutstanding>(ownerId, limits);
        auto updated = std::make_shared<OwnerRegistry>(*registry);
        const auto position = std::lower_bound(
            updated->begin(),
            updated->end(),
            ownerId,
            [](const auto& candidate, std::uint64_t id) {
                return candidate->ownerId < id;
            });
        updated->insert(position, owner);
        std::shared_ptr<const OwnerRegistry> published =
            std::move(updated);
        owners.store(std::move(published), std::memory_order_release);
        return owner;
    }

    BroadcastReason tryReserve(
        const std::shared_ptr<OwnerOutstanding>& owner,
        std::size_t bytes,
        BroadcastPriority priority) noexcept {
        if (!accepting.load(std::memory_order_seq_cst)) {
            return BroadcastReason::OwnerShutdown;
        }

        const auto ownerTasks =
            owner->tasks.tryReserve(1, owner->taskLimit);
        if (!ownerTasks.accepted) {
            return BroadcastReason::OwnerOutstandingTaskLimit;
        }

        const auto ownerBytes =
            owner->bytes.tryReserve(bytes, owner->byteLimit);
        if (!ownerBytes.accepted) {
            owner->tasks.release(1);
            return BroadcastReason::OwnerOutstandingByteLimit;
        }

        const std::size_t effectiveGlobalLimit =
            priority == BroadcastPriority::Low
            ? lowPriorityByteLimit
            : globalByteLimit;
        const auto global =
            globalBytes.tryReserve(bytes, effectiveGlobalLimit);
        if (!global.accepted) {
            owner->bytes.release(bytes);
            owner->tasks.release(1);
            if (priority == BroadcastPriority::Low &&
                fitsWithin(
                    global.observedCurrent,
                    bytes,
                    globalByteLimit)) {
                return BroadcastReason::LowPrioritySoftLimit;
            }
            return BroadcastReason::GlobalOutstandingByteLimit;
        }

        // Sequential consistency gives shutdown and this final check one
        // admission order. A losing reservation rolls every byte/task scope
        // back before reporting OwnerShutdown.
        if (!accepting.load(std::memory_order_seq_cst)) {
            globalBytes.release(bytes);
            owner->bytes.release(bytes);
            owner->tasks.release(1);
            return BroadcastReason::OwnerShutdown;
        }

        const auto aggregateTask = globalTasks.tryReserve(
            1, (std::numeric_limits<std::size_t>::max)());
        if (!aggregateTask.accepted) {
            LOG_FATAL << "Broadcast global outstanding task counter overflow";
        }
        return BroadcastReason::None;
    }

    void release(
        const std::shared_ptr<OwnerOutstanding>& owner,
        std::size_t bytes) noexcept {
        globalBytes.release(bytes);
        owner->bytes.release(bytes);
        owner->tasks.release(1);
        // Keep the legacy aggregate task count as the convergence marker:
        // observing zero with acquire semantics also observes all preceding
        // byte/owner releases for this task.
        globalTasks.release(1);
    }

    DispatchOutstandingSnapshot globalSnapshot() const noexcept {
        return {
            .tasks = globalTasks.current(),
            .bytes = globalBytes.current(),
            .peakTasks = globalTasks.peak(),
            .peakBytes = globalBytes.peak(),
            .rejectedGlobalByteReservations = globalBytes.rejected(),
            .accepting = accepting.load(std::memory_order_seq_cst),
        };
    }

    std::vector<DispatchOwnerOutstandingSnapshot>
    ownerSnapshots() const {
        const auto registry = owners.load(std::memory_order_acquire);
        std::vector<DispatchOwnerOutstandingSnapshot> snapshots;
        snapshots.reserve(registry->size());
        for (const auto& owner : *registry) {
            snapshots.push_back({
                .ownerId = owner->ownerId,
                .tasks = owner->tasks.current(),
                .bytes = owner->bytes.current(),
                .peakTasks = owner->tasks.peak(),
                .peakBytes = owner->bytes.peak(),
                .rejectedTaskReservations =
                    owner->tasks.rejected(),
                .rejectedByteReservations =
                    owner->bytes.rejected(),
            });
        }
        return snapshots;
    }

    static std::shared_ptr<OwnerOutstanding> findOwner(
        const OwnerRegistry& registry,
        std::uint64_t ownerId) noexcept {
        const auto found = std::lower_bound(
            registry.begin(),
            registry.end(),
            ownerId,
            [](const auto& candidate, std::uint64_t id) {
                return candidate->ownerId < id;
            });
        if (found == registry.end() ||
            (*found)->ownerId != ownerId) {
            return {};
        }
        return *found;
    }

    const std::size_t globalByteLimit;
    const std::size_t lowPriorityByteLimit;
    AtomicLimit globalTasks;
    AtomicLimit globalBytes;
    std::atomic<bool> accepting{true};
    std::atomic<std::shared_ptr<const OwnerRegistry>> owners;
    std::mutex ownerRegistrationMutex;
};

BroadcastDispatcher::BroadcastDispatcher(
    DispatchLimits limits,
    BroadcastMetricCallback metricCallback)
    : limits_(limits),
      metricCallback_(std::move(metricCallback)) {
    if (limits_.maxEndpointsPerTask == 0 || limits_.maxBytesPerTask == 0 ||
        limits_.maxOutstandingTasksPerOwner == 0 ||
        limits_.maxOutstandingBytesPerOwner == 0 ||
        limits_.maxGlobalOutstandingBytes == 0 ||
        limits_.lowPriorityOutstandingBytes > limits_.maxGlobalOutstandingBytes) {
        throw std::invalid_argument("BroadcastDispatcher requires coherent positive limits");
    }
    state_ = std::make_shared<State>(limits_);
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

        if (validEndpoints.empty()) {
            continue;
        }

        const auto ownerId = batch.ownerExecutor.id();
        const auto ownerOutstanding =
            state_->ownerFor(ownerId, limits_);

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

            auto payload = plan.payload_;
            auto metricCallback = metricCallback_;
            const auto dispatcherState = state_;
            const auto released =
                std::make_shared<std::atomic<bool>>(false);
            std::function<void()> releaseReservation =
                [dispatcherState,
                 progressState,
                 ownerOutstanding,
                 released,
                 bytes] {
                    if (released->exchange(
                            true, std::memory_order_acq_rel)) {
                        return;
                    }
                    dispatcherState->release(ownerOutstanding, bytes);
                    std::lock_guard lock(progressState->mutex);
                    --progressState->snapshot.outstandingTasks;
                    progressState->snapshot.outstandingBytes -= bytes;
                    progressState->snapshot.complete =
                        progressState->sealed &&
                        progressState->snapshot.outstandingTasks == 0;
                };

            const BroadcastReason reservationFailure =
                state_->tryReserve(
                    ownerOutstanding,
                    bytes,
                    plan.priority_);
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
            ReservationRollback rollback(releaseReservation);

            const auto posted = batch.ownerExecutor.post(
                [payload = std::move(payload),
                 endpoints = std::move(chunk),
                 metricCallback = std::move(metricCallback),
                 progressState,
                 releaseReservation]() mutable {
                    struct ReleaseGuard {
                        std::function<void()> release;
                        ~ReleaseGuard() { release(); }
                    } guard{std::move(releaseReservation)};

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
                rollback.dismiss();
                const auto reason = postFailureReason(posted);
                for (std::size_t index = offset; index < end; ++index) {
                    recordImmediateDrop(reason, validEndpoints[index]->id());
                }
                continue;
            }
            rollback.dismiss();

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
    return state_->globalSnapshot();
}

DispatchOutstandingStats
BroadcastDispatcher::outstandingStats() const {
    return {
        .global = state_->globalSnapshot(),
        .owners = state_->ownerSnapshots(),
    };
}

void BroadcastDispatcher::shutdown() noexcept {
    state_->accepting.store(false, std::memory_order_seq_cst);
}

bool BroadcastDispatcher::accepting() const noexcept {
    return state_->accepting.load(std::memory_order_seq_cst);
}

}  // namespace gamenet::broadcast
