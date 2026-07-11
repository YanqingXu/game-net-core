#pragma once

#include "gamenet/transport/TransportEndpoint.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gamenet::broadcast {

class BroadcastRouter;
class BroadcastDispatcher;

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
    OwnerUnavailable,
    InvalidPlan,
    SendRejected,
};

struct BroadcastMetric {
    BroadcastMetricEvent event{BroadcastMetricEvent::Routed};
    BroadcastReason reason{BroadcastReason::None};
    gamenet::transport::TransportSessionId transportId{};
    std::size_t payloadBytes{};
};

using BroadcastMetricCallback = std::function<void(const BroadcastMetric&)>;

class BroadcastPlan {
public:
    BroadcastPlan(const BroadcastPlan&) = delete;
    BroadcastPlan& operator=(const BroadcastPlan&) = delete;
    BroadcastPlan(BroadcastPlan&&) noexcept = default;
    BroadcastPlan& operator=(BroadcastPlan&&) noexcept = default;

    const std::shared_ptr<const std::string>& payload() const noexcept { return payload_; }
    std::size_t batchCount() const noexcept { return batches_.size(); }
    std::size_t accepted() const noexcept { return accepted_; }
    std::size_t dropped() const noexcept { return dropped_; }

private:
    struct LoopBatch {
        gamenet::net::EventLoopExecutor ownerExecutor;
        std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> endpoints;
    };

    BroadcastPlan() = default;

    std::shared_ptr<const std::string> payload_;
    std::vector<LoopBatch> batches_;
    std::size_t accepted_{};
    std::size_t dropped_{};

    friend class BroadcastRouter;
    friend class BroadcastDispatcher;
};

}  // namespace gamenet::broadcast
