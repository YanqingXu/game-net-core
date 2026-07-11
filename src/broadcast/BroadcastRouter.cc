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
    std::span<const std::shared_ptr<gamenet::game_session::PlayerSession>> targets,
    BroadcastPriority priority) const {
    managementLoop_->assertInLoopThread();
    if (!payload) {
        throw std::invalid_argument("BroadcastRouter requires a shared payload");
    }

    BroadcastPlan plan;
    plan.payload = std::move(payload);
    std::unordered_set<std::uint64_t> seen;
    std::unordered_map<gamenet::net::EventLoop*, std::size_t> batchIndexes;

    for (const auto& session : targets) {
        if (!session || session->state() != gamenet::game_session::SessionState::Online) {
            ++plan.dropped;
            emit(
                BroadcastMetricEvent::Dropped,
                BroadcastReason::OfflineSession,
                session ? session->transportId() : gamenet::transport::TransportSessionId{},
                plan.payload->size());
            continue;
        }
        const auto endpoint = session->endpoint();
        const auto id = endpoint->id();
        if (!seen.insert(id.value).second) {
            ++plan.dropped;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::DuplicateEndpoint, id, plan.payload->size());
            continue;
        }
        if (plan.accepted >= limits_.hardFanout) {
            ++plan.dropped;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::FanoutHardLimit, id, plan.payload->size());
            continue;
        }
        if (plan.payload->size() > limits_.hardBytes / (plan.accepted + 1)) {
            ++plan.dropped;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::ByteHardLimit, id, plan.payload->size());
            continue;
        }
        if (priority == BroadcastPriority::Low &&
            (plan.accepted >= limits_.softFanout ||
             (plan.accepted + 1 != 0 &&
              plan.payload->size() > limits_.softBytes / (plan.accepted + 1)))) {
            ++plan.dropped;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::LowPrioritySoftLimit, id, plan.payload->size());
            continue;
        }
        if (!endpoint->isOpen() || !endpoint->ownerLoop()) {
            ++plan.dropped;
            emit(BroadcastMetricEvent::Dropped, BroadcastReason::EndpointClosed, id, plan.payload->size());
            continue;
        }

        auto [found, inserted] = batchIndexes.try_emplace(endpoint->ownerLoop(), plan.batches.size());
        if (inserted) {
            plan.batches.push_back({.ownerLoop = endpoint->ownerLoop()});
        }
        plan.batches[found->second].endpoints.push_back(endpoint);
        ++plan.accepted;
        emit(BroadcastMetricEvent::Routed, BroadcastReason::None, id, plan.payload->size());
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
