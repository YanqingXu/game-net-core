// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp6/AdaptivePhaseScheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace gamenet::experimental::hp6;

constexpr std::size_t phaseIndex(Phase phase) noexcept {
    return static_cast<std::size_t>(phase);
}

struct Options {
    std::size_t ioItems{32'768};
    std::size_t sideItems{512};
};

std::size_t parsePositive(std::string_view value, std::string_view option) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    if (consumed != value.size() || parsed == 0 ||
        parsed > (std::numeric_limits<std::size_t>::max)()) {
        throw std::invalid_argument(std::string(option) + " requires a positive size");
    }
    return static_cast<std::size_t>(parsed);
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
        const std::string_view argument(argv[argumentIndex]);
        const auto value = [&]() {
            if (++argumentIndex >= argc) {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            return std::string_view(argv[argumentIndex]);
        };
        if (argument == "--io-items") {
            options.ioItems = parsePositive(value(), argument);
        } else if (argument == "--side-items") {
            options.sideItems = parsePositive(value(), argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

struct RunMetrics {
    std::uint64_t plannerElapsedNs{};
    std::uint64_t simulatedCostNs{};
    std::uint64_t maxRoundCostNs{};
    std::uint64_t controlFirstServiceNs{(std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t lifecycleFirstServiceNs{(std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t maximumObservedAgeNs{};
    std::uint64_t checksum{};
    std::uint64_t rounds{};
    std::uint64_t zeroPollContinuations{};
    std::uint64_t completedItems{};
    bool settled{};
};

using Backlogs = std::array<std::size_t, kPhaseCount>;
constexpr std::array<std::uint64_t, kPhaseCount> kCosts{
    100'000, 5'000, 5'000, 5'000, 5'000};

Backlogs initialBacklogs(const Options& options) {
    return Backlogs{
        options.ioItems,
        options.sideItems,
        options.sideItems,
        options.sideItems,
        options.sideItems,
    };
}

bool hasWork(const Backlogs& backlogs) {
    return std::ranges::any_of(backlogs, [](std::size_t value) { return value != 0; });
}

void applyRound(
    const std::array<std::size_t, kPhaseCount>& scheduled,
    Backlogs& backlogs,
    RunMetrics& metrics) {
    std::uint64_t roundCost = 0;
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        if (scheduled[phase] != 0 && phase == phaseIndex(Phase::Control) &&
            metrics.controlFirstServiceNs == (std::numeric_limits<std::uint64_t>::max)()) {
            metrics.controlFirstServiceNs = metrics.simulatedCostNs + roundCost;
        }
        if (scheduled[phase] != 0 && phase == phaseIndex(Phase::Lifecycle) &&
            metrics.lifecycleFirstServiceNs == (std::numeric_limits<std::uint64_t>::max)()) {
            metrics.lifecycleFirstServiceNs = metrics.simulatedCostNs + roundCost;
        }
        backlogs[phase] -= scheduled[phase];
        metrics.completedItems += scheduled[phase];
        metrics.checksum += scheduled[phase] * (phase + 1U);
        roundCost += scheduled[phase] * kCosts[phase];
    }
    metrics.simulatedCostNs += roundCost;
    metrics.maxRoundCostNs = (std::max)(metrics.maxRoundCostNs, roundCost);
    ++metrics.rounds;
}

RunMetrics runFixedCount(const Options& options) {
    auto backlogs = initialBacklogs(options);
    RunMetrics metrics;
    const auto started = Clock::now();
    while (hasWork(backlogs)) {
        std::array<std::size_t, kPhaseCount> scheduled{};
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            scheduled[phase] = (std::min)(backlogs[phase], std::size_t{64});
            if (backlogs[phase] != 0) {
                metrics.maximumObservedAgeNs =
                    (std::max)(metrics.maximumObservedAgeNs, metrics.simulatedCostNs);
            }
        }
        applyRound(scheduled, backlogs, metrics);
        if (hasWork(backlogs)) ++metrics.zeroPollContinuations;
    }
    metrics.plannerElapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.settled = !hasWork(backlogs);
    return metrics;
}

AdaptiveSchedulerOptions candidateOptions() {
    AdaptiveSchedulerOptions options;
    for (auto& phase : options.phases) {
        phase = PhasePolicy{
            .maxItemsPerRound = 64,
            .timeBudgetNs = 200'000,
            .weight = 16,
            .maxDeficit = 128,
            .ageBoostThresholdNs = 2'000'000,
            .ageBoostCredit = 16,
        };
    }
    options.phases[phaseIndex(Phase::Io)].timeBudgetNs = 500'000;
    options.phases[phaseIndex(Phase::Io)].weight = 8;
    return options;
}

RunMetrics runAdaptive(const Options& options) {
    auto backlogs = initialBacklogs(options);
    AdaptivePhaseScheduler scheduler(candidateOptions());
    RunMetrics metrics;
    const auto started = Clock::now();
    while (hasWork(backlogs)) {
        std::array<PhaseObservation, kPhaseCount> observations{};
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            observations[phase] = PhaseObservation{
                .backlog = backlogs[phase],
                .oldestAgeNs = backlogs[phase] == 0 ? 0 : metrics.simulatedCostNs,
                .estimatedItemCostNs = backlogs[phase] == 0 ? 0 : kCosts[phase],
            };
        }
        const auto plan = scheduler.planRound(observations);
        if (plan.status != PlanStatus::Ready || plan.totalScheduled == 0) {
            throw std::runtime_error("adaptive planner made no progress");
        }
        std::array<std::size_t, kPhaseCount> scheduled{};
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            scheduled[phase] = plan.phases[phase].scheduled;
        }
        metrics.maximumObservedAgeNs =
            (std::max)(metrics.maximumObservedAgeNs, plan.maximumOldestAgeNs);
        applyRound(scheduled, backlogs, metrics);
        if (plan.forceNonBlockingPoll) ++metrics.zeroPollContinuations;
    }
    if (scheduler.beginStop() != PlanStatus::Ready) {
        throw std::runtime_error("adaptive planner stop failed");
    }
    metrics.plannerElapsedNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    metrics.settled = scheduler.tryShutdown() && !hasWork(backlogs) &&
        scheduler.snapshot().scheduledItems == metrics.completedItems;
    return metrics;
}

void writeRun(std::string_view name, const RunMetrics& metrics) {
    std::cout << "    \"" << name << "\": {\n"
              << "      \"planner_elapsed_ns\": " << metrics.plannerElapsedNs << ",\n"
              << "      \"simulated_cost_ns\": " << metrics.simulatedCostNs << ",\n"
              << "      \"max_round_cost_ns\": " << metrics.maxRoundCostNs << ",\n"
              << "      \"control_first_service_ns\": " << metrics.controlFirstServiceNs << ",\n"
              << "      \"lifecycle_first_service_ns\": " << metrics.lifecycleFirstServiceNs << ",\n"
              << "      \"maximum_observed_age_ns\": " << metrics.maximumObservedAgeNs << ",\n"
              << "      \"rounds\": " << metrics.rounds << ",\n"
              << "      \"zero_poll_continuations\": " << metrics.zeroPollContinuations << ",\n"
              << "      \"completed_items\": " << metrics.completedItems << ",\n"
              << "      \"checksum\": " << metrics.checksum << ",\n"
              << "      \"shutdown_residue\": " << (metrics.settled ? 0 : 1) << "\n"
              << "    }";
}

int run(int argc, char* argv[]) {
    const auto options = parseOptions(argc, argv);
    Options warmup{.ioItems = 64, .sideItems = 8};
    const auto warmupFixed = runFixedCount(warmup);
    const auto warmupAdaptive = runAdaptive(warmup);
    if (warmupFixed.checksum != warmupAdaptive.checksum || !warmupAdaptive.settled) {
        throw std::runtime_error("HP6 warmup invariant failed");
    }

    const auto fixed = runFixedCount(options);
    const auto adaptive = runAdaptive(options);
    const bool valid = fixed.settled && adaptive.settled &&
        fixed.completedItems == adaptive.completedItems &&
        fixed.simulatedCostNs == adaptive.simulatedCostNs &&
        fixed.checksum == adaptive.checksum &&
        adaptive.maxRoundCostNs < fixed.maxRoundCostNs &&
        adaptive.controlFirstServiceNs < fixed.controlFirstServiceNs &&
        adaptive.lifecycleFirstServiceNs < fixed.lifecycleFirstServiceNs;

    std::cout << "{\n"
              << "  \"schema\": \"gamenet.hp6_adaptive_scheduler_prestudy.v1\",\n"
              << "  \"evidence_class\": \"development-only-synthetic-cost\",\n"
              << "  \"production_event_loop_path\": false,\n"
              << "  \"hp6_b_pool\": \"SKIPPED-BY-EVIDENCE\",\n"
              << "  \"hp6_c_hot_cold\": \"SKIPPED-BY-EVIDENCE\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"io_items\": " << options.ioItems << ",\n"
              << "  \"side_items_per_phase\": " << options.sideItems << ",\n"
              << "  \"runs\": {\n";
    writeRun("fixed_count", fixed);
    std::cout << ",\n";
    writeRun("adaptive_time_deficit", adaptive);
    std::cout << "\n  },\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    return valid ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "gamenet_hp6_adaptive_scheduler_benchmark: "
                  << error.what() << '\n';
        return 2;
    }
}
