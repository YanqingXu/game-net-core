#pragma once

#include "gamenet/broadcast/BroadcastTypes.h"

#include <cstddef>

namespace gamenet::broadcast {

struct DispatchLimits {
    std::size_t maxEndpointsPerTask{64};
    std::size_t maxBytesPerTask{256U * 1024U};
};

struct DispatchSummary {
    std::size_t scheduledEndpoints{};
    std::size_t scheduledTasks{};
};

class BroadcastDispatcher {
public:
    explicit BroadcastDispatcher(
        DispatchLimits limits = {},
        BroadcastMetricCallback metricCallback = {});

    DispatchSummary dispatch(BroadcastPlan plan) const;

private:
    DispatchLimits limits_;
    BroadcastMetricCallback metricCallback_;
};

}  // namespace gamenet::broadcast
