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
    std::span<const std::shared_ptr<const gamenet::game_session::PlayerSession>> targets,
    BroadcastPriority priority) const {
    managementLoop_->assertInLoopThread();
    if (!payload) {
        throw std::invalid_argument("BroadcastRouter requires a shared payload");
    }

    BroadcastPlan plan;
    plan.payload_ = std::move(payload);
    std::unordered_set<std::uint64_t> seen;
    std::unordered_map<std::uint64_t, std::size_t> batchIndexes;

    for (const auto& session : targets) {
        if (!session || session->state() != gamenet::game_session::SessionState::Online) {
            ++plan.dropped_;
            emit(
                BroadcastMetricEvent::Dropped,
                BroadcastReason::OfflineSession,
                session ? session->transportId() : gamenet::transport::TransportSessionId{},
                plan.payload_->size());
            continue;
        }
        const auto endpoint = session->endpoint();
        const auto id = endpoint->id();
        if (!seen.insert(id.value).second) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::DuplicateEndpoint, id, plan.payload_->size());
            continue;
        }
        if (plan.accepted_ >= limits_.hardFanout) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::FanoutHardLimit, id, plan.payload_->size());
            continue;
        }
        if (plan.payload_->size() > limits_.hardBytes / (plan.accepted_ + 1)) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::ByteHardLimit, id, plan.payload_->size());
            continue;
        }
        if (priority == BroadcastPriority::Low &&
            (plan.accepted_ >= limits_.softFanout ||
             (plan.accepted_ + 1 != 0 &&
              plan.payload_->size() > limits_.softBytes / (plan.accepted_ + 1)))) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::LowPrioritySoftLimit, id, plan.payload_->size());
            continue;
        }
        const auto ownerExecutor = endpoint->ownerExecutor();
        if (!ownerExecutor.available()) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::OwnerUnavailable, id, plan.payload_->size());
            continue;
        }
        if (!endpoint->isOpen()) {
            ++plan.dropped_;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::EndpointClosed, id, plan.payload_->size());
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
        metricCallback_({.event = event, .reason = reason, .transportId = id, .payloadBytes = payloadBytes});
    }
}

}  // namespace gamenet::broadcast
