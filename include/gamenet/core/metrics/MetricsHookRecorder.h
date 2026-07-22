#pragma once

// MetricsHookRecorder 将现有 Core metric hook 映射为稳定指标名。
// 返回的回调只共享 exporter，不延长任何 reactor 对象的生命周期。

#include "gamenet/core/metrics/MetricsExporter.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/TcpServer.h"

#include <memory>

namespace gamenet::metrics {

class MetricsHookRecorder {
public:
    explicit MetricsHookRecorder(std::shared_ptr<MetricsExporter> exporter);

    gamenet::net::ConnectorEventCallback makeConnectorCallback() const;
    gamenet::net::EventLoopMetricCallback makeEventLoopCallback() const;
    gamenet::net::TcpServerAdmissionMetricCallback makeTcpServerAdmissionCallback() const;

    static void record(MetricsExporter& exporter, gamenet::net::ConnectorEvent event);
    static void record(MetricsExporter& exporter, const gamenet::net::EventLoopMetricSample& sample);
    static void record(MetricsExporter& exporter, const gamenet::net::TcpServerAdmissionMetric& sample);

private:
    std::shared_ptr<MetricsExporter> exporter_;
};

}  // namespace gamenet::metrics

