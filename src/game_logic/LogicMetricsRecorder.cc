#include "gamenet/game_logic/LogicMetricsRecorder.h"

#include <stdexcept>
#include <utility>

namespace gamenet::game_logic {

LogicLoop::MetricCallback makeLogicMetricsCallback(
    std::shared_ptr<gamenet::metrics::MetricsExporter> exporter) {
    if (!exporter) {
        throw std::invalid_argument("makeLogicMetricsCallback requires a non-null exporter");
    }
    return [exporter = std::move(exporter)](const LogicTickMetric& sample) {
        try {
            exporter->incrementCounter("gamenet.game_logic.tick_completed");
            exporter->observeHistogram("gamenet.game_logic.tick", static_cast<double>(sample.tick));
            exporter->observeHistogram("gamenet.game_logic.commands_drained", static_cast<double>(sample.drained));
            exporter->observeHistogram("gamenet.game_logic.commands_produced", static_cast<double>(sample.produced));
            exporter->observeHistogram("gamenet.game_logic.queue_depth", static_cast<double>(sample.queue.depth));
            exporter->observeHistogram("gamenet.game_logic.queue_bytes", static_cast<double>(sample.queue.queuedBytes));
            exporter->observeHistogram(
                "gamenet.game_logic.queue_depth_high_watermark",
                static_cast<double>(sample.queue.depthHighWatermark));
            exporter->observeHistogram(
                "gamenet.game_logic.queue_bytes_high_watermark",
                static_cast<double>(sample.queue.bytesHighWatermark));
        } catch (...) {
            // Observability failure must not alter the logic tick.
        }
    };
}

}  // namespace gamenet::game_logic
