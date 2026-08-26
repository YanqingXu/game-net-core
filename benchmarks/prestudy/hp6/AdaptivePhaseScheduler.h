// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace gamenet::experimental::hp6 {

enum class Phase : std::size_t {
    Io,
    Timer,
    Control,
    Lifecycle,
    Functor,
    Count,
};

inline constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::Count);

struct PhasePolicy {
    std::size_t maxItemsPerRound{64};
    std::uint64_t timeBudgetNs{500'000};
    std::size_t weight{8};
    std::size_t maxDeficit{128};
    std::uint64_t ageBoostThresholdNs{2'000'000};
    std::size_t ageBoostCredit{8};
};

struct AdaptiveSchedulerOptions {
    std::array<PhasePolicy, kPhaseCount> phases{};

    AdaptiveSchedulerOptions();
    void validate() const;
};

struct PhaseObservation {
    std::size_t backlog{};
    std::uint64_t oldestAgeNs{};
    std::uint64_t estimatedItemCostNs{};
};

struct PhaseDecision {
    std::size_t scheduled{};
    std::size_t remaining{};
    std::uint64_t estimatedCostNs{};
    bool countExhausted{};
    bool timeExhausted{};
    bool deficitExhausted{};
    bool ageBoosted{};
};

enum class PlanStatus {
    Ready,
    Stopped,
    WrongOwner,
    InvalidObservation,
};

struct RoundPlan {
    PlanStatus status{PlanStatus::InvalidObservation};
    std::array<PhaseDecision, kPhaseCount> phases{};
    std::size_t totalScheduled{};
    std::size_t totalRemaining{};
    std::uint64_t estimatedTotalCostNs{};
    std::uint64_t maximumOldestAgeNs{};
    bool forceNonBlockingPoll{};
};

struct AdaptiveSchedulerSnapshot {
    std::uint64_t rounds{};
    std::uint64_t scheduledItems{};
    bool stopped{};
    bool inPlan{};

    bool settled() const noexcept { return stopped && !inPlan; }
};

class AdaptivePhaseScheduler final {
public:
    explicit AdaptivePhaseScheduler(AdaptiveSchedulerOptions options = {});

    AdaptivePhaseScheduler(const AdaptivePhaseScheduler&) = delete;
    AdaptivePhaseScheduler& operator=(const AdaptivePhaseScheduler&) = delete;

    RoundPlan planRound(
        const std::array<PhaseObservation, kPhaseCount>& observations) noexcept;
    PlanStatus beginStop() noexcept;
    bool tryShutdown() const noexcept;
    AdaptiveSchedulerSnapshot snapshot() const noexcept;

private:
    bool isOwner() const noexcept;

    AdaptiveSchedulerOptions options_;
    std::array<std::size_t, kPhaseCount> deficits_{};
    std::thread::id owner_;
    std::uint64_t rounds_{};
    std::uint64_t scheduledItems_{};
    bool stopped_{};
    bool inPlan_{};
};

}  // namespace gamenet::experimental::hp6
