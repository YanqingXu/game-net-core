#include "gamenet/broadcast/BroadcastMetricsRecorder.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace gamenet::broadcast {

namespace {

std::string_view eventName(BroadcastMetricEvent event) noexcept {
    switch (event) {
    case BroadcastMetricEvent::Routed: return "routed";
    case BroadcastMetricEvent::Scheduled: return "scheduled";
    case BroadcastMetricEvent::Sent: return "sent";
    case BroadcastMetricEvent::Dropped: return "dropped";
    }
    return "unknown";
}

std::string_view reasonName(BroadcastReason reason) noexcept {
    switch (reason) {
    case BroadcastReason::None: return "none";
    case BroadcastReason::OfflineSession: return "offline_session";
    case BroadcastReason::DuplicateEndpoint: return "duplicate_endpoint";
    case BroadcastReason::FanoutHardLimit: return "fanout_hard_limit";
    case BroadcastReason::ByteHardLimit: return "byte_hard_limit";
    case BroadcastReason::LowPrioritySoftLimit: return "low_priority_soft_limit";
    case BroadcastReason::DispatchTaskByteLimit: return "dispatch_task_byte_limit";
    case BroadcastReason::EndpointClosed: return "endpoint_closed";
    case BroadcastReason::OwnerUnavailable: return "owner_unavailable";
    case BroadcastReason::InvalidPlan: return "invalid_plan";
    case BroadcastReason::SendRejected: return "send_rejected";
    }
    return "unknown";
}

std::string metricName(std::string_view category, std::string_view value) {
    std::string result("gamenet.broadcast.");
    result.append(category);
    result.push_back('.');
    result.append(value);
    return result;
}

}  // namespace

BroadcastMetricCallback makeBroadcastMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter) {
    if (!exporter) {
        throw std::invalid_argument("makeBroadcastMetricsCallback requires a non-null exporter");
    }
    return [exporter = std::move(exporter)](const BroadcastMetric& sample) {
        try {
            exporter->incrementCounter(metricName("event", eventName(sample.event)));
            exporter->incrementCounter(metricName("reason", reasonName(sample.reason)));
            exporter->observeHistogram(
                "gamenet.broadcast.payload_bytes",
                static_cast<double>(sample.payloadBytes));
        } catch (...) {
            // Observability failure must not alter routing or dispatch.
        }
    };
}

}  // namespace gamenet::broadcast
