#pragma once

// Provisional adapter：同步将 LogicLoop tick 样本记录到共享 MetricsExporter；
// 当前实现不承诺无分配或无锁热路径。

#include "gamenet/core/metrics/MetricsExporter.h"
#include "gamenet/game_logic/LogicLoop.h"

#include <memory>

namespace gamenet::game_logic {

LogicLoop::MetricCallback makeLogicMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter);

}  // namespace gamenet::game_logic

