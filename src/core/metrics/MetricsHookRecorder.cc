#include "gamenet/core/metrics/MetricsHookRecorder.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace gamenet::metrics {

namespace {

std::string_view connectorEventName(gamenet::net::ConnectorEvent event) noexcept {
    using gamenet::net::ConnectorEvent;
    switch (event) {
    case ConnectorEvent::ConnectAttempt: return "connect_attempt";
    case ConnectorEvent::ConnectSuccess: return "connect_success";
    case ConnectorEvent::ConnectFailed: return "connect_failed";
    case ConnectorEvent::RetryScheduled: return "retry_scheduled";
    case ConnectorEvent::SelfConnectDetected: return "self_connect_detected";
    case ConnectorEvent::ConnectTimeout: return "connect_timeout";
    }
    return "unknown";
}

std::string_view eventLoopEventName(gamenet::net::EventLoopMetricEvent event) noexcept {
    using gamenet::net::EventLoopMetricEvent;
    switch (event) {
    case EventLoopMetricEvent::PendingFunctorsDrained: return "pending_functors_drained";
    case EventLoopMetricEvent::WakeupHandled: return "wakeup_handled";
    }
    return "unknown";
}

std::string_view admissionEventName(gamenet::net::TcpServerAdmissionEvent event) noexcept {
    using gamenet::net::TcpServerAdmissionEvent;
    switch (event) {
    case TcpServerAdmissionEvent::Accepted: return "accepted";
    case TcpServerAdmissionEvent::RejectedConnectionLimit: return "rejected_connection_limit";
    case TcpServerAdmissionEvent::RejectedPerPeerLimit: return "rejected_per_peer_limit";
    case TcpServerAdmissionEvent::RejectedPerPeerRateLimit: return "rejected_per_peer_rate_limit";
    case TcpServerAdmissionEvent::RejectedPeerTrackingCapacity: return "rejected_peer_tracking_capacity";
    case TcpServerAdmissionEvent::Authenticated: return "authenticated";
    case TcpServerAdmissionEvent::AuthenticationTimedOut: return "authentication_timed_out";
    }
    return "unknown";
}

std::string eventCounter(std::string_view prefix, std::string_view event) {
    std::string name(prefix);
    name.push_back('.');
    name.append(event);
    return name;
}

}  // namespace

MetricsHookRecorder::MetricsHookRecorder(std::shared_ptr<MetricsExporter> exporter)
    : exporter_(std::move(exporter)) {
    if (!exporter_) {
        throw std::invalid_argument("MetricsHookRecorder requires a non-null exporter");
    }
}

gamenet::net::ConnectorEventCallback MetricsHookRecorder::makeConnectorCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const gamenet::net::InetAddress&, auto event) {
        try {
            record(*exporter, event);
        } catch (...) {
            // Observability failure must not alter connector behavior.
        }
    };
}

gamenet::net::EventLoopMetricCallback MetricsHookRecorder::makeEventLoopCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const auto& sample) {
        try {
            record(*exporter, sample);
        } catch (...) {
            // Observability failure must not alter owner-loop behavior.
        }
    };
}

gamenet::net::TcpServerAdmissionMetricCallback
MetricsHookRecorder::makeTcpServerAdmissionCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const auto& sample) {
        try {
            record(*exporter, sample);
        } catch (...) {
            // Observability failure must not alter admission behavior.
        }
    };
}

void MetricsHookRecorder::record(MetricsExporter& exporter, gamenet::net::ConnectorEvent event) {
    exporter.incrementCounter(eventCounter("gamenet.net.connector", connectorEventName(event)));
}

void MetricsHookRecorder::record(
    MetricsExporter& exporter,
    const gamenet::net::EventLoopMetricSample& sample) {
    exporter.incrementCounter(eventCounter("gamenet.net.event_loop", eventLoopEventName(sample.event)));
    exporter.observeHistogram("gamenet.net.event_loop.pending_functors", static_cast<double>(sample.pendingFunctors));
    exporter.observeHistogram("gamenet.net.event_loop.pending_functor_peak", static_cast<double>(sample.pendingFunctorPeak));
    exporter.observeHistogram("gamenet.net.event_loop.wakeup_count", static_cast<double>(sample.wakeupCount));
    exporter.observeHistogram("gamenet.net.event_loop.rejected_functors", static_cast<double>(sample.rejectedFunctors));
    exporter.observeHistogram("gamenet.net.event_loop.callback_exceptions", static_cast<double>(sample.callbackExceptions));
    exporter.observeHistogram(
        "gamenet.net.event_loop.oldest_pending_latency_seconds",
        std::chrono::duration<double>(sample.oldestPendingLatency).count());
}

void MetricsHookRecorder::record(
    MetricsExporter& exporter,
    const gamenet::net::TcpServerAdmissionMetric& sample) {
    exporter.incrementCounter(eventCounter("gamenet.net.tcp_server.admission", admissionEventName(sample.event)));
    exporter.observeHistogram(
        "gamenet.net.tcp_server.active_connections",
        static_cast<double>(sample.activeConnections));
    exporter.observeHistogram(
        "gamenet.net.tcp_server.active_peer_connections",
        static_cast<double>(sample.activePeerConnections));
}

}  // namespace gamenet::metrics
