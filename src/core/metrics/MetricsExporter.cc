#include "gamenet/core/metrics/MetricsExporter.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gamenet::metrics {

namespace {

bool validNameStart(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           value == '_' || value == ':';
}

bool validNameRest(char value) noexcept {
    return validNameStart(value) || (value >= '0' && value <= '9') || value == '.';
}

void validateMetricBaseName(std::string_view name) {
    if (name.empty() || !validNameStart(name.front())) {
        throw std::invalid_argument("metric name must start with [A-Za-z_:]");
    }
    if (!std::all_of(name.begin(), name.end(), validNameRest)) {
        throw std::invalid_argument("metric name contains an invalid character");
    }
}

bool validLabelStart(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

bool validLabelRest(char value) noexcept {
    return validLabelStart(value) || (value >= '0' && value <= '9');
}

MetricLabels canonicalLabels(MetricLabels labels) {
    for (const auto& label : labels) {
        if (label.key.empty() || !validLabelStart(label.key.front()) ||
            !std::all_of(label.key.begin(), label.key.end(), validLabelRest)) {
            throw std::invalid_argument("metric label key must match [A-Za-z_][A-Za-z0-9_]*");
        }
    }
    std::sort(labels.begin(), labels.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });
    if (std::adjacent_find(labels.begin(), labels.end(), [](const auto& left, const auto& right) {
            return left.key == right.key;
        }) != labels.end()) {
        throw std::invalid_argument("metric labels must not contain duplicate keys");
    }
    return labels;
}

std::string escapeLabelValue(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '"':
            escaped.append("\\\"");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::string labelSuffix(const MetricLabels& labels) {
    if (labels.empty()) {
        return {};
    }
    std::string suffix{"{"};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (index != 0) {
            suffix.push_back(',');
        }
        suffix.append(labels[index].key);
        suffix.append("=\"");
        suffix.append(escapeLabelValue(labels[index].value));
        suffix.push_back('"');
    }
    suffix.push_back('}');
    return suffix;
}

struct ParsedName {
    std::string_view base;
    std::string_view labels;
};

ParsedName parseName(std::string_view name) {
    const auto open = name.find('{');
    if (open == std::string_view::npos) {
        validateMetricBaseName(name);
        return {name, {}};
    }
    if (open == 0 || name.back() != '}' || name.find('{', open + 1) != std::string_view::npos ||
        name.find('}', open) != name.size() - 1) {
        throw std::invalid_argument("metric label suffix is malformed");
    }
    validateMetricBaseName(name.substr(0, open));
    if (name.find('\n', open) != std::string_view::npos || name.find('\r', open) != std::string_view::npos) {
        throw std::invalid_argument("metric label suffix contains a raw newline");
    }
    return {name.substr(0, open), name.substr(open)};
}

std::string sanitizePrometheusName(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        result.push_back(character == '.' ? '_' : character);
    }
    return result;
}

std::string formatDouble(double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

void emitTypeOnce(std::ostringstream& output,
                  std::unordered_set<std::string>& emitted,
                  std::string_view name,
                  std::string_view type) {
    std::string identity(name);
    identity.push_back(':');
    identity.append(type);
    if (emitted.insert(std::move(identity)).second) {
        output << "# TYPE " << name << ' ' << type << '\n';
    }
}

}  // namespace

void InMemoryMetricsExporter::incrementCounter(std::string_view name, std::uint64_t delta) {
    parseName(name);
    std::lock_guard lock(mutex_);
    auto& value = counters_[std::string(name)];
    const auto available = std::numeric_limits<std::uint64_t>::max() - value;
    value += std::min(delta, available);
}

void InMemoryMetricsExporter::observeHistogram(std::string_view name, double value) {
    parseName(name);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("histogram observation must be finite");
    }
    std::lock_guard lock(mutex_);
    auto key = std::string(name);
    auto& aggregate = histograms_[key];
    if (aggregate.name.empty()) {
        aggregate.name = std::move(key);
    }
    if (aggregate.count == 0) {
        aggregate.min = value;
        aggregate.max = value;
    } else {
        aggregate.min = std::min(aggregate.min, value);
        aggregate.max = std::max(aggregate.max, value);
    }
    if (aggregate.count != std::numeric_limits<std::uint64_t>::max()) {
        ++aggregate.count;
    }
    aggregate.sum += value;
}

std::uint64_t InMemoryMetricsExporter::counterValue(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto found = counters_.find(std::string(name));
    return found == counters_.end() ? 0 : found->second;
}

HistogramSnapshot InMemoryMetricsExporter::histogram(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto found = histograms_.find(std::string(name));
    if (found != histograms_.end()) {
        return found->second;
    }
    return HistogramSnapshot{.name = std::string(name)};
}

MetricsSnapshot InMemoryMetricsExporter::snapshot() const {
    std::lock_guard lock(mutex_);
    MetricsSnapshot result;
    result.counters.reserve(counters_.size());
    result.histograms.reserve(histograms_.size());
    for (const auto& [name, value] : counters_) {
        result.counters.push_back({name, value});
    }
    for (const auto& [name, histogram] : histograms_) {
        (void)name;
        result.histograms.push_back(histogram);
    }
    std::sort(result.counters.begin(), result.counters.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::sort(result.histograms.begin(), result.histograms.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return result;
}

void InMemoryMetricsExporter::reset() {
    std::lock_guard lock(mutex_);
    counters_.clear();
    histograms_.clear();
}

TaggedMetricsExporter::TaggedMetricsExporter(
    std::shared_ptr<MetricsExporter> sink,
    MetricLabels labels)
    : sink_(std::move(sink)), labels_(canonicalLabels(std::move(labels))), labelSuffix_(labelSuffix(labels_)) {
    if (!sink_) {
        throw std::invalid_argument("TaggedMetricsExporter requires a non-null sink");
    }
}

void TaggedMetricsExporter::incrementCounter(std::string_view name, std::uint64_t delta) {
    validateMetricBaseName(name);
    std::string tagged(name);
    tagged.append(labelSuffix_);
    sink_->incrementCounter(tagged, delta);
}

void TaggedMetricsExporter::observeHistogram(std::string_view name, double value) {
    validateMetricBaseName(name);
    std::string tagged(name);
    tagged.append(labelSuffix_);
    sink_->observeHistogram(tagged, value);
}

std::string metricNameWithLabels(std::string_view name, const MetricLabels& labels) {
    validateMetricBaseName(name);
    std::string result(name);
    result.append(labelSuffix(canonicalLabels(labels)));
    return result;
}

std::string renderPrometheusText(const MetricsSnapshot& snapshot) {
    std::ostringstream output;
    std::unordered_set<std::string> emittedTypes;
    for (const auto& counter : snapshot.counters) {
        const auto parsed = parseName(counter.name);
        const auto base = sanitizePrometheusName(parsed.base);
        emitTypeOnce(output, emittedTypes, base, "counter");
        output << base << parsed.labels << ' ' << counter.value << '\n';
    }
    for (const auto& histogram : snapshot.histograms) {
        const auto parsed = parseName(histogram.name);
        const auto base = sanitizePrometheusName(parsed.base);
        emitTypeOnce(output, emittedTypes, base, "summary");
        output << base << "_count" << parsed.labels << ' ' << histogram.count << '\n';
        output << base << "_sum" << parsed.labels << ' ' << formatDouble(histogram.sum) << '\n';
        emitTypeOnce(output, emittedTypes, base + "_min", "gauge");
        emitTypeOnce(output, emittedTypes, base + "_max", "gauge");
        output << base << "_min" << parsed.labels << ' ' << formatDouble(histogram.min) << '\n';
        output << base << "_max" << parsed.labels << ' ' << formatDouble(histogram.max) << '\n';
    }
    return output.str();
}

}  // namespace gamenet::metrics

