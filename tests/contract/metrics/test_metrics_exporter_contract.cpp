#include "gamenet/core/metrics/MetricsHookRecorder.h"
#include "gamenet/core/net/EventLoop.h"

#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class ThrowingExporter final : public gamenet::metrics::MetricsExporter {
public:
    void incrementCounter(std::string_view, std::uint64_t) override {
        throw std::runtime_error("counter sink failed");
    }
    void observeHistogram(std::string_view, double) override {
        throw std::runtime_error("histogram sink failed");
    }
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using gamenet::metrics::InMemoryMetricsExporter;
    using gamenet::metrics::MetricsHookRecorder;
    using gamenet::metrics::TaggedMetricsExporter;

    auto exporter = std::make_shared<InMemoryMetricsExporter>();
    gamenet::net::ConnectorEventCallback connectorCallback;
    gamenet::net::EventLoopMetricCallback eventLoopCallback;
    gamenet::net::TcpServerAdmissionMetricCallback admissionCallback;
    {
        MetricsHookRecorder recorder(exporter);
        connectorCallback = recorder.makeConnectorCallback();
        eventLoopCallback = recorder.makeEventLoopCallback();
        admissionCallback = recorder.makeTcpServerAdmissionCallback();
    }

    connectorCallback(gamenet::net::InetAddress(1), gamenet::net::ConnectorEvent::ConnectSuccess);
    eventLoopCallback({
        .event = gamenet::net::EventLoopMetricEvent::PendingFunctorsDrained,
        .pendingFunctors = 3,
        .pendingFunctorPeak = 5,
        .wakeupCount = 7,
        .rejectedFunctors = 2,
        .callbackExceptions = 1,
        .oldestPendingLatency = 4ms,
    });
    admissionCallback({
        .event = gamenet::net::TcpServerAdmissionEvent::RejectedPerPeerRateLimit,
        .activeConnections = 10,
        .activePeerConnections = 2,
    });
    GAMENET_TEST_ASSERT(exporter->counterValue("gamenet.net.connector.connect_success") == 1);
    GAMENET_TEST_ASSERT(
        exporter->counterValue("gamenet.net.event_loop.pending_functors_drained") == 1);
    GAMENET_TEST_ASSERT(exporter->histogram("gamenet.net.event_loop.pending_functors").max == 3);
    GAMENET_TEST_ASSERT(exporter->counterValue(
        "gamenet.net.tcp_server.admission.rejected_per_peer_rate_limit") == 1);

    constexpr int threadCount = 8;
    constexpr int recordsPerThread = 2000;
    auto tagged = std::make_shared<TaggedMetricsExporter>(
        exporter,
        gamenet::metrics::MetricLabels{{"service", "contract"}});
    std::vector<std::thread> threads;
    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([exporter, tagged] {
            for (int record = 0; record < recordsPerThread; ++record) {
                exporter->incrementCounter("gamenet.test.concurrent");
                tagged->incrementCounter("gamenet.test.tagged_concurrent");
                tagged->observeHistogram("gamenet.test.thread_value", 1.0);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto expected = static_cast<std::uint64_t>(threadCount * recordsPerThread);
    GAMENET_TEST_ASSERT(exporter->counterValue("gamenet.test.concurrent") == expected);
    GAMENET_TEST_ASSERT(exporter->counterValue(
        "gamenet.test.tagged_concurrent{service=\"contract\"}") == expected);
    GAMENET_TEST_ASSERT(exporter->histogram(
        "gamenet.test.thread_value{service=\"contract\"}").count == expected);

    bool nullRejected = false;
    try {
        MetricsHookRecorder invalid(nullptr);
    } catch (const std::invalid_argument&) {
        nullRejected = true;
    }
    GAMENET_TEST_ASSERT(nullRejected);

    MetricsHookRecorder throwingRecorder(std::make_shared<ThrowingExporter>());
    throwingRecorder.makeConnectorCallback()(
        gamenet::net::InetAddress(1),
        gamenet::net::ConnectorEvent::ConnectFailed);
    throwingRecorder.makeEventLoopCallback()({});
    throwingRecorder.makeTcpServerAdmissionCallback()({});

    gamenet::net::EventLoop loop;
    loop.setEventLoopMetricCallback(MetricsHookRecorder(exporter).makeEventLoopCallback());
    loop.queueInLoop([] {});
    loop.runAfter(5ms, [&loop] { loop.quit(); });
    loop.loop();
    GAMENET_TEST_ASSERT(
        exporter->counterValue("gamenet.net.event_loop.pending_functors_drained") >= 2);
}
