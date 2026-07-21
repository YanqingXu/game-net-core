#pragma once

// MetricsExporter 提供与 EventLoop 无关的线程安全指标聚合与快照导出。
// 热路径只记录内存状态；文本序列化由报告线程显式执行。

#include "gamenet/core/base/noncopyable.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gamenet::metrics {

struct CounterSnapshot {
    std::string name;
    std::uint64_t value{0};
};

struct HistogramSnapshot {
    std::string name;
    std::uint64_t count{0};
    double sum{0.0};
    double min{0.0};
    double max{0.0};

    double average() const noexcept {
        return count == 0 ? 0.0 : sum / static_cast<double>(count);
    }
};

struct MetricsSnapshot {
    std::vector<CounterSnapshot> counters;
    std::vector<HistogramSnapshot> histograms;
};

struct MetricLabel {
    std::string key;
    std::string value;

    bool operator==(const MetricLabel&) const = default;
};

using MetricLabels = std::vector<MetricLabel>;

class MetricsExporter : private gamenet::base::noncopyable {
public:
    virtual ~MetricsExporter() = default;

    virtual void incrementCounter(std::string_view name, std::uint64_t delta = 1) = 0;
    virtual void observeHistogram(std::string_view name, double value) = 0;
};

class InMemoryMetricsExporter final : public MetricsExporter {
public:
    void incrementCounter(std::string_view name, std::uint64_t delta = 1) override;
    void observeHistogram(std::string_view name, double value) override;

    std::uint64_t counterValue(std::string_view name) const;
    HistogramSnapshot histogram(std::string_view name) const;
    MetricsSnapshot snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> counters_;
    std::unordered_map<std::string, HistogramSnapshot> histograms_;
};

class TaggedMetricsExporter final : public MetricsExporter {
public:
    TaggedMetricsExporter(std::shared_ptr<MetricsExporter> sink, MetricLabels labels);

    void incrementCounter(std::string_view name, std::uint64_t delta = 1) override;
    void observeHistogram(std::string_view name, double value) override;

    const MetricLabels& labels() const noexcept { return labels_; }

private:
    std::shared_ptr<MetricsExporter> sink_;
    MetricLabels labels_;
    std::string labelSuffix_;
};

std::string metricNameWithLabels(std::string_view name, const MetricLabels& labels);
std::string renderPrometheusText(const MetricsSnapshot& snapshot);

}  // namespace gamenet::metrics

