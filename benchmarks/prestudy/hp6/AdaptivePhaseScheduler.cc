// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp6/AdaptivePhaseScheduler.h"

#include <algorithm>
#include <stdexcept>

namespace gamenet::experimental::hp6 {

AdaptiveSchedulerOptions::AdaptiveSchedulerOptions() {
    for (auto& phase : phases) phase = PhasePolicy{};
}

void AdaptiveSchedulerOptions::validate() const {
    for (const auto& phase : phases) {
        if (phase.maxItemsPerRound == 0 || phase.timeBudgetNs == 0 ||
            phase.weight == 0 || phase.maxDeficit < phase.weight ||
            phase.ageBoostCredit > phase.maxDeficit) {
            throw std::invalid_argument("invalid adaptive phase policy");
        }
    }
}

AdaptivePhaseScheduler::AdaptivePhaseScheduler(AdaptiveSchedulerOptions options)
    : options_(options), owner_(std::this_thread::get_id()) {
    options_.validate();
}

RoundPlan AdaptivePhaseScheduler::planRound(
    const std::array<PhaseObservation, kPhaseCount>& observations) noexcept {
    RoundPlan plan;
    if (!isOwner()) {
        plan.status = PlanStatus::WrongOwner;
        return plan;
    }
    if (stopped_) {
        plan.status = PlanStatus::Stopped;
        return plan;
    }
    for (const auto& observation : observations) {
        if (observation.backlog != 0 && observation.estimatedItemCostNs == 0) {
            plan.status = PlanStatus::InvalidObservation;
            return plan;
        }
    }

    inPlan_ = true;
    plan.status = PlanStatus::Ready;
    for (std::size_t index = 0; index < kPhaseCount; ++index) {
        const auto& policy = options_.phases[index];
        const auto& observation = observations[index];
        auto& decision = plan.phases[index];
        plan.maximumOldestAgeNs =
            (std::max)(plan.maximumOldestAgeNs, observation.oldestAgeNs);

        std::size_t credit = policy.weight;
        decision.ageBoosted = observation.backlog != 0 &&
            observation.oldestAgeNs >= policy.ageBoostThresholdNs;
        if (decision.ageBoosted) {
            credit = (std::min)(
                policy.maxDeficit,
                credit + policy.ageBoostCredit);
        }
        deficits_[index] = (std::min)(
            policy.maxDeficit,
            deficits_[index] + credit);

        const auto countAndDeficitQuota = (std::min)({
            observation.backlog,
            policy.maxItemsPerRound,
            deficits_[index],
        });
        std::size_t timeQuota = 0;
        if (observation.backlog != 0) {
            timeQuota = static_cast<std::size_t>(
                policy.timeBudgetNs / observation.estimatedItemCostNs);
            if (timeQuota == 0) timeQuota = 1;
        }
        decision.scheduled = (std::min)(countAndDeficitQuota, timeQuota);
        decision.remaining = observation.backlog - decision.scheduled;
        decision.estimatedCostNs =
            decision.scheduled * observation.estimatedItemCostNs;
        decision.countExhausted = decision.remaining != 0 &&
            decision.scheduled == policy.maxItemsPerRound;
        decision.timeExhausted = decision.remaining != 0 &&
            decision.scheduled == timeQuota && timeQuota < countAndDeficitQuota;
        decision.deficitExhausted = decision.remaining != 0 &&
            decision.scheduled == deficits_[index] &&
            deficits_[index] < policy.maxItemsPerRound;
        deficits_[index] -= decision.scheduled;

        plan.totalScheduled += decision.scheduled;
        plan.totalRemaining += decision.remaining;
        plan.estimatedTotalCostNs += decision.estimatedCostNs;
    }
    plan.forceNonBlockingPoll = plan.totalRemaining != 0;
    ++rounds_;
    scheduledItems_ += plan.totalScheduled;
    inPlan_ = false;
    return plan;
}

PlanStatus AdaptivePhaseScheduler::beginStop() noexcept {
    if (!isOwner()) return PlanStatus::WrongOwner;
    stopped_ = true;
    return PlanStatus::Ready;
}

bool AdaptivePhaseScheduler::tryShutdown() const noexcept {
    return isOwner() && stopped_ && !inPlan_;
}

AdaptiveSchedulerSnapshot AdaptivePhaseScheduler::snapshot() const noexcept {
    if (!isOwner()) return {};
    return AdaptiveSchedulerSnapshot{
        .rounds = rounds_,
        .scheduledItems = scheduledItems_,
        .stopped = stopped_,
        .inPlan = inPlan_,
    };
}

bool AdaptivePhaseScheduler::isOwner() const noexcept {
    return owner_ == std::this_thread::get_id();
}

}  // namespace gamenet::experimental::hp6
