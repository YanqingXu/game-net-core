// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp6/AdaptivePhaseScheduler.h"

#include "support/TestAssert.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>

namespace {

using namespace gamenet::experimental::hp6;

constexpr std::size_t index(Phase phase) noexcept {
    return static_cast<std::size_t>(phase);
}

std::array<PhaseObservation, kPhaseCount> emptyObservations() {
    return {};
}

}  // namespace

int main() {
    using namespace gamenet::experimental::hp6;

    bool rejectedOptions = false;
    try {
        AdaptiveSchedulerOptions invalid;
        invalid.phases[index(Phase::Io)].timeBudgetNs = 0;
        AdaptivePhaseScheduler scheduler(invalid);
    } catch (const std::invalid_argument&) {
        rejectedOptions = true;
    }
    GAMENET_TEST_ASSERT(rejectedOptions);

    AdaptiveSchedulerOptions boundedOptions;
    auto& ioPolicy = boundedOptions.phases[index(Phase::Io)];
    ioPolicy.maxItemsPerRound = 10;
    ioPolicy.timeBudgetNs = 250;
    ioPolicy.weight = 10;
    ioPolicy.maxDeficit = 20;
    ioPolicy.ageBoostThresholdNs = 1'000;
    ioPolicy.ageBoostCredit = 5;
    AdaptivePhaseScheduler bounded(boundedOptions);
    auto observations = emptyObservations();
    observations[index(Phase::Io)] = PhaseObservation{
        .backlog = 20,
        .oldestAgeNs = 0,
        .estimatedItemCostNs = 100,
    };
    auto plan = bounded.planRound(observations);
    GAMENET_TEST_ASSERT(plan.status == PlanStatus::Ready);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].scheduled == 2);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].remaining == 18);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].timeExhausted);
    GAMENET_TEST_ASSERT(plan.forceNonBlockingPoll);

    observations[index(Phase::Io)].estimatedItemCostNs = 1'000;
    plan = bounded.planRound(observations);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].scheduled == 1);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].timeExhausted);

    observations[index(Phase::Io)].estimatedItemCostNs = 1;
    observations[index(Phase::Io)].oldestAgeNs = 2'000;
    plan = bounded.planRound(observations);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].scheduled == 10);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].countExhausted);
    GAMENET_TEST_ASSERT(plan.phases[index(Phase::Io)].ageBoosted);

    auto invalidObservation = emptyObservations();
    invalidObservation[index(Phase::Timer)].backlog = 1;
    GAMENET_TEST_ASSERT(
        bounded.planRound(invalidObservation).status ==
        PlanStatus::InvalidObservation);

    // Every saturated phase receives its configured bounded weighted share.
    AdaptiveSchedulerOptions weightedOptions;
    const std::array<std::size_t, kPhaseCount> weights{5, 4, 3, 2, 1};
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        weightedOptions.phases[phase] = PhasePolicy{
            .maxItemsPerRound = 32,
            .timeBudgetNs = 1'000,
            .weight = weights[phase],
            .maxDeficit = 64,
            .ageBoostThresholdNs = 1'000'000,
            .ageBoostCredit = 0,
        };
    }
    AdaptivePhaseScheduler weighted(weightedOptions);
    std::array<std::size_t, kPhaseCount> totals{};
    auto saturated = emptyObservations();
    for (auto& observation : saturated) {
        observation = PhaseObservation{
            .backlog = 1'000,
            .oldestAgeNs = 0,
            .estimatedItemCostNs = 1,
        };
    }
    for (std::size_t round = 0; round < 20; ++round) {
        const auto weightedPlan = weighted.planRound(saturated);
        GAMENET_TEST_ASSERT(weightedPlan.status == PlanStatus::Ready);
        GAMENET_TEST_ASSERT(weightedPlan.forceNonBlockingPoll);
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            totals[phase] += weightedPlan.phases[phase].scheduled;
        }
        GAMENET_TEST_ASSERT(
            weightedPlan.phases[index(Phase::Control)].scheduled >= 1);
        GAMENET_TEST_ASSERT(
            weightedPlan.phases[index(Phase::Lifecycle)].scheduled >= 1);
    }
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        GAMENET_TEST_ASSERT(totals[phase] == weights[phase] * 20);
    }

    std::atomic<PlanStatus> foreignStatus{PlanStatus::Ready};
    std::jthread foreign([&] {
        foreignStatus.store(
            weighted.planRound(saturated).status,
            std::memory_order_release);
    });
    foreign.join();
    GAMENET_TEST_ASSERT(
        foreignStatus.load(std::memory_order_acquire) == PlanStatus::WrongOwner);

    GAMENET_TEST_ASSERT(weighted.beginStop() == PlanStatus::Ready);
    GAMENET_TEST_ASSERT(
        weighted.planRound(saturated).status == PlanStatus::Stopped);
    GAMENET_TEST_ASSERT(weighted.tryShutdown());
    GAMENET_TEST_ASSERT(weighted.snapshot().settled());
    GAMENET_TEST_ASSERT(bounded.beginStop() == PlanStatus::Ready);
    GAMENET_TEST_ASSERT(bounded.tryShutdown());

    return 0;
}
