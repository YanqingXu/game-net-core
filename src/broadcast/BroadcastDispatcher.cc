#include "gamenet/broadcast/BroadcastDispatcher.h"

#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::broadcast {

BroadcastDispatcher::BroadcastDispatcher(
    DispatchLimits limits,
    BroadcastMetricCallback metricCallback)
    : limits_(limits), metricCallback_(std::move(metricCallback)) {
    if (limits_.maxEndpointsPerTask == 0 || limits_.maxBytesPerTask == 0) {
        throw std::invalid_argument("BroadcastDispatcher requires positive task limits");
    }
}

DispatchSummary BroadcastDispatcher::dispatch(BroadcastPlan plan) const {
    DispatchSummary summary;
    if (!plan.payload) return summary;

    if (plan.payload->size() > limits_.maxBytesPerTask) {
        if (metricCallback_) {
            for (const auto& batch : plan.batches) {
                for (const auto& endpoint : batch.endpoints) {
                    metricCallback_({
                        .event = BroadcastMetricEvent::Dropped,
                        .reason = BroadcastReason::DispatchTaskByteLimit,
                        .transportId = endpoint->id(),
                        .payloadBytes = plan.payload->size(),
                    });
                }
            }
        }
        return summary;
    }

    const auto byBytes = plan.payload->empty()
        ? limits_.maxEndpointsPerTask
        : std::max<std::size_t>(1, limits_.maxBytesPerTask / plan.payload->size());
    const auto chunkSize = std::min(limits_.maxEndpointsPerTask, byBytes);

    for (auto& batch : plan.batches) {
        for (std::size_t offset = 0; offset < batch.endpoints.size(); offset += chunkSize) {
            const auto end = std::min(batch.endpoints.size(), offset + chunkSize);
            std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> chunk(
                batch.endpoints.begin() + static_cast<std::ptrdiff_t>(offset),
                batch.endpoints.begin() + static_cast<std::ptrdiff_t>(end));
            summary.scheduledEndpoints += chunk.size();
            ++summary.scheduledTasks;
            auto payload = plan.payload;
            auto metricCallback = metricCallback_;
            batch.ownerLoop->queueInLoop(
                [payload = std::move(payload),
                 endpoints = std::move(chunk),
                 metricCallback = std::move(metricCallback)] {
                    for (const auto& endpoint : endpoints) {
                        BroadcastMetric metric{
                            .event = BroadcastMetricEvent::Sent,
                            .reason = BroadcastReason::None,
                            .transportId = endpoint->id(),
                            .payloadBytes = payload->size(),
                        };
                        if (!endpoint->isOpen()) {
                            metric.event = BroadcastMetricEvent::Dropped;
                            metric.reason = BroadcastReason::EndpointClosed;
                        } else if (endpoint->send(*payload) != gamenet::transport::EndpointResult::Accepted) {
                            metric.event = BroadcastMetricEvent::Dropped;
                            metric.reason = BroadcastReason::SendRejected;
                        }
                        if (metricCallback) metricCallback(metric);
                    }
                });

            if (metricCallback_) {
                for (std::size_t index = offset; index < end; ++index) {
                    metricCallback_({
                        .event = BroadcastMetricEvent::Scheduled,
                        .reason = BroadcastReason::None,
                        .transportId = batch.endpoints[index]->id(),
                        .payloadBytes = plan.payload->size(),
                    });
                }
            }
        }
    }
    return summary;
}

}  // namespace gamenet::broadcast
