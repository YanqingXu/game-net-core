#pragma once

// Provisional adapter：同步记录广播结果与丢弃原因；
// 高 fanout 下的分配、哈希和锁开销尚未成为稳定性能合同。

#include "gamenet/broadcast/BroadcastTypes.h"
#include "gamenet/core/metrics/MetricsExporter.h"

#include <memory>

namespace gamenet::broadcast {

BroadcastMetricCallback makeBroadcastMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter);

}  // namespace gamenet::broadcast

