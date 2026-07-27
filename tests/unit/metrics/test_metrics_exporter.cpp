#include "gamenet/core/metrics/MetricsExporter.h"

#include "support/TestAssert.h"

#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

template <typename Function>
bool throwsInvalidArgument(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    using namespace gamenet::metrics;

    InMemoryMetricsExporter exporter;
    GAMENET_TEST_ASSERT(exporter.counterValue("missing.counter") == 0);
    GAMENET_TEST_ASSERT(exporter.histogram("missing.histogram").count == 0);

    exporter.incrementCounter("gamenet.test.events");
    exporter.incrementCounter("gamenet.test.events", 4);
    exporter.observeHistogram("gamenet.test.latency_seconds", 0.25);
    exporter.observeHistogram("gamenet.test.latency_seconds", 0.75);
    GAMENET_TEST_ASSERT(exporter.counterValue("gamenet.test.events") == 5);
    const auto histogram = exporter.histogram("gamenet.test.latency_seconds");
    GAMENET_TEST_ASSERT(histogram.count == 2);
    GAMENET_TEST_ASSERT(histogram.sum == 1.0);
    GAMENET_TEST_ASSERT(histogram.min == 0.25);
    GAMENET_TEST_ASSERT(histogram.max == 0.75);
    GAMENET_TEST_ASSERT(histogram.average() == 0.5);

    const MetricLabels labels{{"zone", "east\n1"}, {"service", "game\"server"}};
    auto sink = std::make_shared<InMemoryMetricsExporter>();
    TaggedMetricsExporter tagged(sink, labels);
    GAMENET_TEST_ASSERT(tagged.labels()[0].key == "service");
    GAMENET_TEST_ASSERT(tagged.labels()[1].key == "zone");
    tagged.incrementCounter("gamenet.test.tagged");
    tagged.observeHistogram("gamenet.test.bytes", 128.0);
    const auto taggedCounter = metricNameWithLabels("gamenet.test.tagged", labels);
    const auto taggedHistogram = metricNameWithLabels("gamenet.test.bytes", labels);
    GAMENET_TEST_ASSERT(sink->counterValue(taggedCounter) == 1);
    GAMENET_TEST_ASSERT(sink->histogram(taggedHistogram).max == 128.0);

    const auto snapshot = sink->snapshot();
    const auto text = renderPrometheusText(snapshot);
    GAMENET_TEST_ASSERT(text.find("# TYPE gamenet_test_tagged counter") != std::string::npos);
    GAMENET_TEST_ASSERT(text.find(
        "gamenet_test_tagged{service=\"game\\\"server\",zone=\"east\\n1\"} 1") !=
        std::string::npos);
    GAMENET_TEST_ASSERT(text.find("gamenet_test_bytes_count") != std::string::npos);
    GAMENET_TEST_ASSERT(text.find("gamenet_test_bytes_sum") != std::string::npos);

    GAMENET_TEST_ASSERT(throwsInvalidArgument([] {
        TaggedMetricsExporter invalid(nullptr, {});
    }));
    GAMENET_TEST_ASSERT(throwsInvalidArgument([&] {
        TaggedMetricsExporter invalid(sink, {{"duplicate", "a"}, {"duplicate", "b"}});
    }));
    GAMENET_TEST_ASSERT(throwsInvalidArgument([&] {
        TaggedMetricsExporter invalid(sink, {{"bad-key", "a"}});
    }));
    GAMENET_TEST_ASSERT(throwsInvalidArgument([&] {
        exporter.incrementCounter("bad metric");
    }));
    GAMENET_TEST_ASSERT(throwsInvalidArgument([&] {
        exporter.observeHistogram("gamenet.test.invalid", std::numeric_limits<double>::infinity());
    }));

    exporter.reset();
    GAMENET_TEST_ASSERT(exporter.counterValue("gamenet.test.events") == 0);
    GAMENET_TEST_ASSERT(exporter.snapshot().histograms.empty());
}

