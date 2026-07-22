#pragma once

// BroadcastMetricsRecorder 将广播结果和丢弃原因记录到共享 MetricsExporter。

#include "gamenet/broadcast/BroadcastTypes.h"
#include "gamenet/core/metrics/MetricsExporter.h"

#include <memory>

namespace gamenet::broadcast {

BroadcastMetricCallback makeBroadcastMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter);

}  // namespace gamenet::broadcast

