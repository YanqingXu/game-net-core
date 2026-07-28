#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/EventLoop.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gamenet::broadcast {

BroadcastRouter::BroadcastRouter(
    gamenet::net::EventLoop* managementLoop,
    BroadcastLimits limits,
    BroadcastMetricCallback metricCallback)
    : managementLoop_(managementLoop), limits_(limits), metricCallback_(std::move(metricCallback)) {
    if (!managementLoop_ || limits_.softFanout > limits_.hardFanout ||
        limits_.softBytes > limits_.hardBytes || limits_.hardFanout == 0 || limits_.hardBytes == 0) {
        throw std::invalid_argument("BroadcastRouter requires coherent limits and management loop");
    }
}

BroadcastPlan BroadcastRouter::route(
    std::shared_ptr<const std::string> payload,
    std::span<const BroadcastTarget> targets,
    BroadcastPriority priority) const {
    managementLoop_->assertInLoopThread();
    if (!payload) {
        throw std::invalid_argument("BroadcastRouter requires a shared payload");
    }

    BroadcastPlan plan;
    plan.payload_ = std::move(payload);
    plan.priority_ = priority;
    std::unordered_set<std::uint64_t> seen;
    std::unordered_map<std::uint64_t, std::size_t> batchIndexes;
    const auto drop = [this, &plan](
                          BroadcastReason reason,
                          gamenet::transport::TransportSessionId id) {
        ++plan.dropped_;
        ++plan.reasonCounts_[reasonIndex(reason)];
        emit(
            BroadcastMetricEvent::Dropped,
            reason,
            id,
            plan.payload_->size());
    };

    for (const auto& target : targets) {
        if (!target.eligible()) {
            drop(BroadcastReason::OfflineSession, target.id());
            continue;
        }
        const auto endpoint = target.endpoint();
        if (!endpoint) {
            drop(BroadcastReason::InvalidPlan, {});
            continue;
        }
        const auto id = endpoint->id();
        if (!seen.insert(id.value).second) {
            drop(BroadcastReason::DuplicateEndpoint, id);
            continue;
        }
        if (plan.accepted_ >= limits_.hardFanout) {
            drop(BroadcastReason::FanoutHardLimit, id);
            continue;
        }
        if (plan.payload_->size() > limits_.hardBytes / (plan.accepted_ + 1)) {
            drop(BroadcastReason::ByteHardLimit, id);
            continue;
        }
        if (priority == BroadcastPriority::Low &&
            (plan.accepted_ >= limits_.softFanout ||
             (plan.accepted_ + 1 != 0 &&
              plan.payload_->size() > limits_.softBytes / (plan.accepted_ + 1)))) {
            drop(BroadcastReason::LowPrioritySoftLimit, id);
            continue;
        }
        const auto ownerExecutor = endpoint->ownerExecutor();
        if (!ownerExecutor.available()) {
            drop(BroadcastReason::OwnerUnavailable, id);
            continue;
        }
        if (!endpoint->isOpen()) {
            drop(BroadcastReason::EndpointClosed, id);
            continue;
        }

        auto [found, inserted] = batchIndexes.try_emplace(ownerExecutor.id(), plan.batches_.size());
        if (inserted) {
            plan.batches_.push_back({.ownerExecutor = ownerExecutor});
        }
        plan.batches_[found->second].endpoints.push_back(endpoint);
        ++plan.accepted_;
        emit(BroadcastMetricEvent::Routed, BroadcastReason::None, id, plan.payload_->size());
    }
    return plan;
}

void BroadcastRouter::emit(
    BroadcastMetricEvent event,
    BroadcastReason reason,
    gamenet::transport::TransportSessionId id,
    std::size_t payloadBytes) const {
    if (metricCallback_) {
        try {
            metricCallback_({
                .event = event,
                .reason = reason,
                .transportId = id,
                .payloadBytes = payloadBytes});
        } catch (...) {
            // Metrics are observational and must not alter routing admission.
        }
    }
}

}  // namespace gamenet::broadcast
