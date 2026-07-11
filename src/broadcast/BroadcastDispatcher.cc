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
    if (!plan.payload_) {
        if (metricCallback_) {
            metricCallback_({
                .event = BroadcastMetricEvent::Dropped,
                .reason = BroadcastReason::InvalidPlan,
            });
        }
        return summary;
    }

    const auto emitDropped = [this, &plan](
                                 BroadcastReason reason,
                                 gamenet::transport::TransportSessionId id) {
        if (metricCallback_) {
            metricCallback_({
                .event = BroadcastMetricEvent::Dropped,
                .reason = reason,
                .transportId = id,
                .payloadBytes = plan.payload_->size(),
            });
        }
    };

    if (plan.payload_->size() > limits_.maxBytesPerTask) {
        if (metricCallback_) {
            for (const auto& batch : plan.batches_) {
                for (const auto& endpoint : batch.endpoints) {
                    metricCallback_({
                        .event = BroadcastMetricEvent::Dropped,
                        .reason = BroadcastReason::DispatchTaskByteLimit,
                        .transportId = endpoint ? endpoint->id()
                                                : gamenet::transport::TransportSessionId{},
                        .payloadBytes = plan.payload_->size(),
                    });
                }
            }
        }
        return summary;
    }

    const auto byBytes = plan.payload_->empty()
        ? limits_.maxEndpointsPerTask
        : std::max<std::size_t>(1, limits_.maxBytesPerTask / plan.payload_->size());
    const auto chunkSize = std::min(limits_.maxEndpointsPerTask, byBytes);

    for (auto& batch : plan.batches_) {
        if (!batch.ownerExecutor.available()) {
            for (const auto& endpoint : batch.endpoints) {
                emitDropped(
                    BroadcastReason::OwnerUnavailable,
                    endpoint ? endpoint->id() : gamenet::transport::TransportSessionId{});
            }
            continue;
        }

        std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> validEndpoints;
        validEndpoints.reserve(batch.endpoints.size());
        for (const auto& endpoint : batch.endpoints) {
            if (!endpoint) {
                emitDropped(BroadcastReason::InvalidPlan, {});
                continue;
            }
            const auto endpointExecutor = endpoint->ownerExecutor();
            if (!endpointExecutor.available()) {
                emitDropped(BroadcastReason::OwnerUnavailable, endpoint->id());
                continue;
            }
            if (endpointExecutor.id() != batch.ownerExecutor.id()) {
                emitDropped(BroadcastReason::InvalidPlan, endpoint->id());
                continue;
            }
            if (!endpoint->isOpen()) {
                emitDropped(BroadcastReason::EndpointClosed, endpoint->id());
                continue;
            }
            validEndpoints.push_back(endpoint);
        }

        for (std::size_t offset = 0; offset < validEndpoints.size(); offset += chunkSize) {
            const auto end = std::min(validEndpoints.size(), offset + chunkSize);
            std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> chunk(
                validEndpoints.begin() + static_cast<std::ptrdiff_t>(offset),
                validEndpoints.begin() + static_cast<std::ptrdiff_t>(end));
            summary.scheduledEndpoints += chunk.size();
            ++summary.scheduledTasks;
            auto payload = plan.payload_;
            auto metricCallback = metricCallback_;
            const bool queued = batch.ownerExecutor.tryQueue(
                [payload = std::move(payload),
                 endpoints = std::move(chunk),
                 metricCallback = std::move(metricCallback)] {
                    for (const auto& endpoint : endpoints) {
                        BroadcastMetric metric{
                            .event = BroadcastMetricEvent::Scheduled,
                            .reason = BroadcastReason::None,
                            .transportId = endpoint->id(),
                            .payloadBytes = payload->size(),
                        };
                        if (metricCallback) metricCallback(metric);
                        metric.event = BroadcastMetricEvent::Sent;
                        if (!endpoint->isOpen()) {
                            metric.event = BroadcastMetricEvent::Dropped;
                            metric.reason = BroadcastReason::EndpointClosed;
                        } else {
                            const auto result = endpoint->send(*payload);
                            if (result == gamenet::transport::EndpointResult::Closed) {
                                metric.event = BroadcastMetricEvent::Dropped;
                                metric.reason = BroadcastReason::EndpointClosed;
                            } else if (result == gamenet::transport::EndpointResult::OwnerUnavailable) {
                                metric.event = BroadcastMetricEvent::Dropped;
                                metric.reason = BroadcastReason::OwnerUnavailable;
                            } else if (result != gamenet::transport::EndpointResult::Accepted) {
                                metric.event = BroadcastMetricEvent::Dropped;
                                metric.reason = BroadcastReason::SendRejected;
                            }
                        }
                        if (metricCallback) metricCallback(metric);
                    }
                });

            if (!queued) {
                summary.scheduledEndpoints -= end - offset;
                --summary.scheduledTasks;
                if (metricCallback_) {
                    for (std::size_t index = offset; index < end; ++index) {
                        metricCallback_({
                            .event = BroadcastMetricEvent::Dropped,
                            .reason = BroadcastReason::OwnerUnavailable,
                            .transportId = validEndpoints[index]->id(),
                            .payloadBytes = plan.payload_->size(),
                        });
                    }
                }
                continue;
            }

        }
    }
    return summary;
}

}  // namespace gamenet::broadcast
