#pragma once

// MetricsHookRecorder 将 Core hook 映射到 provisional 指标 schema。
// 回调同步调用 exporter，可能分配或竞争；它只共享 exporter，不延长 reactor 生命周期。

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

