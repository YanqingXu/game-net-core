#pragma once

// LogicMetricsRecorder 将 LogicLoop tick 样本记录到共享 MetricsExporter。

#include "gamenet/core/metrics/MetricsExporter.h"
#include "gamenet/game_logic/LogicLoop.h"

#include <memory>

namespace gamenet::game_logic {

LogicLoop::MetricCallback makeLogicMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter);

}  // namespace gamenet::game_logic

