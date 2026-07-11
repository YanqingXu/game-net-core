#pragma once

#include "gamenet/transport/TransportEndpoint.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::broadcast {

enum class BroadcastPriority : std::uint8_t {
    Low,
    Normal,
    High,
};

enum class BroadcastMetricEvent {
    Routed,
    Scheduled,
    Sent,
    Dropped,
};

enum class BroadcastReason {
    None,
    OfflineSession,
    DuplicateEndpoint,
    FanoutHardLimit,
    ByteHardLimit,
    LowPrioritySoftLimit,
    DispatchTaskByteLimit,
    EndpointClosed,
    SendRejected,
};

struct BroadcastMetric {
    BroadcastMetricEvent event{BroadcastMetricEvent::Routed};
    BroadcastReason reason{BroadcastReason::None};
    gamenet::transport::TransportSessionId transportId{};
    std::size_t payloadBytes{};
};

using BroadcastMetricCallback = std::function<void(const BroadcastMetric&)>;

struct BroadcastLoopBatch {
    gamenet::net::EventLoop* ownerLoop{};
    std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> endpoints;
};

struct BroadcastPlan {
    std::shared_ptr<const std::string> payload;
    std::vector<BroadcastLoopBatch> batches;
    std::size_t accepted{};
    std::size_t dropped{};
};

}  // namespace gamenet::broadcast
