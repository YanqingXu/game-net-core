#pragma once

#include "gamenet/broadcast/BroadcastTypes.h"
#include "gamenet/game_session/PlayerSession.h"

#include <cstddef>
#include <memory>
#include <span>

namespace gamenet::broadcast {

struct BroadcastLimits {
    std::size_t softFanout{512};
    std::size_t hardFanout{1024};
    std::size_t softBytes{512U * 1024U};
    std::size_t hardBytes{1024U * 1024U};
};

class BroadcastRouter {
public:
    BroadcastRouter(
        gamenet::net::EventLoop* managementLoop,
        BroadcastLimits limits = {},
        BroadcastMetricCallback metricCallback = {});

    BroadcastPlan route(
        std::shared_ptr<const std::string> payload,
        std::span<const std::shared_ptr<gamenet::game_session::PlayerSession>> targets,
        BroadcastPriority priority = BroadcastPriority::Normal) const;

private:
    void emit(
        BroadcastMetricEvent event,
        BroadcastReason reason,
        gamenet::transport::TransportSessionId id,
        std::size_t payloadBytes) const;

    gamenet::net::EventLoop* managementLoop_;
    BroadcastLimits limits_;
    BroadcastMetricCallback metricCallback_;
};

}  // namespace gamenet::broadcast
