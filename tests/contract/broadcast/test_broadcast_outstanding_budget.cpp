#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class BudgetEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    BudgetEndpoint(
        std::uint64_t id,
        gamenet::net::EventLoopExecutor executor,
        gamenet::transport::EndpointResult sendResult =
            gamenet::transport::EndpointResult::Accepted,
        bool throwOnSend = false)
        : id_{id},
          executor_(std::move(executor)),
          sendResult_(sendResult),
          throwOnSend_(throwOnSend) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override { return executor_; }
    gamenet::transport::EndpointResult send(std::string_view) override {
        sends_.fetch_add(1, std::memory_order_relaxed);
        if (throwOnSend_) throw std::runtime_error("endpoint send failure");
        return sendResult_;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false, std::memory_order_release);
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }
    std::size_t sends() const noexcept {
        return sends_.load(std::memory_order_acquire);
    }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoopExecutor executor_;
    gamenet::transport::EndpointResult sendResult_;
    bool throwOnSend_{false};
    std::atomic<bool> open_{true};
    std::atomic<std::size_t> sends_{0};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop managementLoop;
    gamenet::net::EventLoopThread ownerThread;
    auto* ownerLoop = ownerThread.startLoop();

    std::promise<void> blocked;
    auto blockedFuture = blocked.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    ownerLoop->queueInLoop([&] {
        blocked.set_value();
        releaseFuture.wait();
    });
    gamenet::test::waitUntilReady(
        blockedFuture, 2s, "broadcast owner did not enter budget barrier");

    auto endpoint = std::make_shared<BudgetEndpoint>(1, ownerLoop->executor());
    std::vector<gamenet::broadcast::BroadcastTarget> targets{
        gamenet::broadcast::BroadcastTarget(endpoint)};
    gamenet::broadcast::BroadcastRouter router(&managementLoop);
    gamenet::broadcast::BroadcastDispatcher dispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = 1,
        .maxOutstandingBytesPerOwner = 4,
        .maxGlobalOutstandingBytes = 4,
        .lowPriorityOutstandingBytes = 2,
    });

    auto firstPlan =
        router.route(std::make_shared<const std::string>("four"), targets);
    auto first = dispatcher.dispatch(std::move(firstPlan));
    GAMENET_TEST_ASSERT(first.scheduledEndpoints == 1);
    GAMENET_TEST_ASSERT(first.acceptedEndpoints == 1);
    GAMENET_TEST_ASSERT(first.droppedEndpoints == 0);

    auto secondPlan =
        router.route(std::make_shared<const std::string>("four"), targets);
    auto second = dispatcher.dispatch(std::move(secondPlan));
    GAMENET_TEST_ASSERT(second.acceptedEndpoints == 0);
    GAMENET_TEST_ASSERT(second.droppedEndpoints == 1);
    GAMENET_TEST_ASSERT(
        second.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerOutstandingTaskLimit) == 1);

    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!first.progress->snapshot().complete || endpoint->sends() != 1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(first.progress->snapshot().acceptedEndpoints == 1);
    const auto outstanding = dispatcher.outstanding();
    GAMENET_TEST_ASSERT(outstanding.tasks == 0);
    GAMENET_TEST_ASSERT(outstanding.bytes == 0);

    auto overloaded = std::make_shared<BudgetEndpoint>(
        2,
        ownerLoop->executor(),
        gamenet::transport::EndpointResult::Overloaded);
    std::vector<gamenet::broadcast::BroadcastTarget> overloadTarget{
        gamenet::broadcast::BroadcastTarget(overloaded)};
    auto overloadPlan =
        router.route(std::make_shared<const std::string>("x"), overloadTarget);
    auto overloadSummary = dispatcher.dispatch(std::move(overloadPlan));
    const auto overloadDeadline = std::chrono::steady_clock::now() + 2s;
    while (!overloadSummary.progress->snapshot().complete &&
           std::chrono::steady_clock::now() < overloadDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(
        overloadSummary.progress->snapshot().reasonCount(
            gamenet::broadcast::BroadcastReason::EndpointOverloaded) == 1);

    std::promise<void> bytesBlocked;
    auto bytesBlockedFuture = bytesBlocked.get_future();
    std::promise<void> releaseBytes;
    auto releaseBytesFuture = releaseBytes.get_future().share();
    ownerLoop->queueInLoop([&] {
        bytesBlocked.set_value();
        releaseBytesFuture.wait();
    });
    gamenet::test::waitUntilReady(
        bytesBlockedFuture, 2s, "broadcast owner did not enter byte barrier");
    gamenet::broadcast::BroadcastDispatcher ownerBytesDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = 4,
        .maxOutstandingBytesPerOwner = 4,
        .maxGlobalOutstandingBytes = 32,
        .lowPriorityOutstandingBytes = 32,
    });
    auto ownerBytesFirst = ownerBytesDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("abc"), targets));
    auto ownerBytesSecond = ownerBytesDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("abc"), targets));
    GAMENET_TEST_ASSERT(ownerBytesFirst.acceptedEndpoints == 1);
    GAMENET_TEST_ASSERT(
        ownerBytesSecond.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerOutstandingByteLimit) == 1);
    releaseBytes.set_value();
    while (!ownerBytesFirst.progress->snapshot().complete) {
        std::this_thread::yield();
    }

    gamenet::net::EventLoopThread secondOwnerThread;
    auto* secondOwnerLoop = secondOwnerThread.startLoop();
    std::promise<void> firstGlobalBlocked;
    std::promise<void> secondGlobalBlocked;
    auto firstGlobalBlockedFuture = firstGlobalBlocked.get_future();
    auto secondGlobalBlockedFuture = secondGlobalBlocked.get_future();
    std::promise<void> releaseGlobal;
    auto releaseGlobalFuture = releaseGlobal.get_future().share();
    ownerLoop->queueInLoop([&] {
        firstGlobalBlocked.set_value();
        releaseGlobalFuture.wait();
    });
    secondOwnerLoop->queueInLoop([&] {
        secondGlobalBlocked.set_value();
        releaseGlobalFuture.wait();
    });
    gamenet::test::waitUntilReady(
        firstGlobalBlockedFuture, 2s, "first global owner did not enter barrier");
    gamenet::test::waitUntilReady(
        secondGlobalBlockedFuture, 2s, "second global owner did not enter barrier");
    auto secondEndpoint =
        std::make_shared<BudgetEndpoint>(3, secondOwnerLoop->executor());
    std::vector<gamenet::broadcast::BroadcastTarget> secondTargets{
        gamenet::broadcast::BroadcastTarget(secondEndpoint)};
    gamenet::broadcast::BroadcastDispatcher globalDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = 4,
        .maxOutstandingBytesPerOwner = 16,
        .maxGlobalOutstandingBytes = 4,
        .lowPriorityOutstandingBytes = 4,
    });
    auto globalFirst = globalDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("four"), targets));
    auto globalSecond = globalDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("four"), secondTargets));
    GAMENET_TEST_ASSERT(globalFirst.acceptedEndpoints == 1);
    GAMENET_TEST_ASSERT(
        globalSecond.reasonCount(
            gamenet::broadcast::BroadcastReason::GlobalOutstandingByteLimit) == 1);
    releaseGlobal.set_value();
    while (!globalFirst.progress->snapshot().complete) {
        std::this_thread::yield();
    }

    gamenet::broadcast::BroadcastDispatcher lowPriorityDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = 4,
        .maxOutstandingBytesPerOwner = 16,
        .maxGlobalOutstandingBytes = 16,
        .lowPriorityOutstandingBytes = 2,
    });
    auto lowPriority = lowPriorityDispatcher.dispatch(router.route(
        std::make_shared<const std::string>("abc"),
        targets,
        gamenet::broadcast::BroadcastPriority::Low));
    GAMENET_TEST_ASSERT(
        lowPriority.reasonCount(
            gamenet::broadcast::BroadcastReason::LowPrioritySoftLimit) == 1);

    gamenet::net::EventLoopThread fullOwnerThread;
    auto* fullOwner = fullOwnerThread.startLoop();
    std::promise<void> fullOwnerBlocked;
    auto fullOwnerBlockedFuture = fullOwnerBlocked.get_future();
    std::promise<void> releaseFullOwner;
    auto releaseFullOwnerFuture = releaseFullOwner.get_future().share();
    fullOwner->queueInLoop([&] {
        fullOwnerBlocked.set_value();
        releaseFullOwnerFuture.wait();
    });
    gamenet::test::waitUntilReady(
        fullOwnerBlockedFuture, 2s, "queue-full owner did not enter barrier");
    auto fullEndpoint =
        std::make_shared<BudgetEndpoint>(4, fullOwner->executor());
    std::vector<gamenet::broadcast::BroadcastTarget> fullTargets{
        gamenet::broadcast::BroadcastTarget(fullEndpoint)};
    auto fullPlan =
        router.route(std::make_shared<const std::string>("x"), fullTargets);
    std::size_t filled = 0;
    while (fullOwner->executor().post([] {}) ==
           gamenet::net::PostResult::Accepted) {
        ++filled;
    }
    GAMENET_TEST_ASSERT(filled != 0);
    gamenet::broadcast::BroadcastDispatcher terminalDispatcher;
    auto fullSummary = terminalDispatcher.dispatch(std::move(fullPlan));
    GAMENET_TEST_ASSERT(
        fullSummary.reasonCount(
            gamenet::broadcast::BroadcastReason::DispatchQueueFull) == 1);
    GAMENET_TEST_ASSERT(terminalDispatcher.outstanding().tasks == 0);
    releaseFullOwner.set_value();
    fullOwnerThread.stop();

    gamenet::net::EventLoopThread shutdownOwnerThread;
    auto* shutdownOwner = shutdownOwnerThread.startLoop();
    std::promise<void> shutdownOwnerBlocked;
    auto shutdownOwnerBlockedFuture = shutdownOwnerBlocked.get_future();
    std::promise<void> releaseShutdownOwner;
    auto releaseShutdownOwnerFuture = releaseShutdownOwner.get_future().share();
    shutdownOwner->queueInLoop([&] {
        shutdownOwnerBlocked.set_value();
        releaseShutdownOwnerFuture.wait();
    });
    gamenet::test::waitUntilReady(
        shutdownOwnerBlockedFuture, 2s, "shutdown owner did not enter barrier");
    auto shutdownEndpoint =
        std::make_shared<BudgetEndpoint>(5, shutdownOwner->executor());
    std::vector<gamenet::broadcast::BroadcastTarget> shutdownTargets{
        gamenet::broadcast::BroadcastTarget(shutdownEndpoint)};
    auto shutdownPlan =
        router.route(std::make_shared<const std::string>("x"), shutdownTargets);
    shutdownOwner->quit();
    auto shutdownSummary = terminalDispatcher.dispatch(std::move(shutdownPlan));
    GAMENET_TEST_ASSERT(
        shutdownSummary.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerShutdown) == 1);
    releaseShutdownOwner.set_value();
    shutdownOwnerThread.stop();

    std::optional<gamenet::broadcast::BroadcastPlan> unavailablePlan;
    std::shared_ptr<BudgetEndpoint> unavailableEndpoint;
    {
        gamenet::net::EventLoopThread unavailableOwnerThread;
        auto* unavailableOwner = unavailableOwnerThread.startLoop();
        unavailableEndpoint =
            std::make_shared<BudgetEndpoint>(6, unavailableOwner->executor());
        std::vector<gamenet::broadcast::BroadcastTarget> unavailableTargets{
            gamenet::broadcast::BroadcastTarget(unavailableEndpoint)};
        unavailablePlan.emplace(router.route(
            std::make_shared<const std::string>("x"), unavailableTargets));
        unavailableOwnerThread.stop();
    }
    auto unavailableSummary =
        terminalDispatcher.dispatch(std::move(*unavailablePlan));
    GAMENET_TEST_ASSERT(
        unavailableSummary.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerUnavailable) == 1);

    auto exceptionEndpoint =
        std::make_shared<BudgetEndpoint>(7, ownerLoop->executor());
    std::vector<gamenet::broadcast::BroadcastTarget> exceptionTargets{
        gamenet::broadcast::BroadcastTarget(exceptionEndpoint)};
    gamenet::broadcast::BroadcastDispatcher exceptionDispatcher(
        {},
        [](const gamenet::broadcast::BroadcastMetric&) {
            throw std::runtime_error("metric failure");
        });
    auto exceptionSummary = exceptionDispatcher.dispatch(router.route(
        std::make_shared<const std::string>("x"), exceptionTargets));
    const auto exceptionDeadline = std::chrono::steady_clock::now() + 2s;
    while (!exceptionSummary.progress->snapshot().complete &&
           std::chrono::steady_clock::now() < exceptionDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(exceptionEndpoint->sends() == 1);
    GAMENET_TEST_ASSERT(exceptionDispatcher.outstanding().tasks == 0);
    GAMENET_TEST_ASSERT(
        exceptionSummary.progress->snapshot().acceptedEndpoints == 1);

    auto throwingEndpoint = std::make_shared<BudgetEndpoint>(
        8,
        ownerLoop->executor(),
        gamenet::transport::EndpointResult::Accepted,
        true);
    std::vector<gamenet::broadcast::BroadcastTarget> throwingTargets{
        gamenet::broadcast::BroadcastTarget(throwingEndpoint)};
    auto throwingSummary = terminalDispatcher.dispatch(router.route(
        std::make_shared<const std::string>("x"), throwingTargets));
    const auto throwingDeadline = std::chrono::steady_clock::now() + 2s;
    while (!throwingSummary.progress->snapshot().complete &&
           std::chrono::steady_clock::now() < throwingDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(
        throwingSummary.progress->snapshot().reasonCount(
            gamenet::broadcast::BroadcastReason::SendRejected) == 1);
    GAMENET_TEST_ASSERT(terminalDispatcher.outstanding().tasks == 0);

    std::promise<void> shutdownBlocked;
    auto shutdownBlockedFuture = shutdownBlocked.get_future();
    std::promise<void> releasePendingBroadcast;
    auto releasePendingBroadcastFuture = releasePendingBroadcast.get_future().share();
    ownerLoop->queueInLoop([&] {
        shutdownBlocked.set_value();
        releasePendingBroadcastFuture.wait();
    });
    gamenet::test::waitUntilReady(
        shutdownBlockedFuture, 2s, "pending broadcast owner did not enter barrier");
    gamenet::broadcast::BroadcastDispatcher stoppingDispatcher;
    auto pendingAtShutdown = stoppingDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("pending"), targets));
    GAMENET_TEST_ASSERT(pendingAtShutdown.acceptedEndpoints == 1);
    stoppingDispatcher.shutdown();
    stoppingDispatcher.shutdown();
    GAMENET_TEST_ASSERT(!stoppingDispatcher.accepting());
    auto afterShutdown = stoppingDispatcher.dispatch(
        router.route(std::make_shared<const std::string>("rejected"), targets));
    GAMENET_TEST_ASSERT(afterShutdown.acceptedEndpoints == 0);
    GAMENET_TEST_ASSERT(
        afterShutdown.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerShutdown) == 1);
    releasePendingBroadcast.set_value();
    const auto pendingDeadline = std::chrono::steady_clock::now() + 2s;
    while (!pendingAtShutdown.progress->snapshot().complete &&
           std::chrono::steady_clock::now() < pendingDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(pendingAtShutdown.progress->snapshot().acceptedEndpoints == 1);
    GAMENET_TEST_ASSERT(stoppingDispatcher.outstanding().tasks == 0);

    constexpr std::size_t kConcurrentPlans = 32;
    constexpr std::size_t kConcurrentLimit = 8;

    std::promise<void> atomicOwnerBlocked;
    auto atomicOwnerBlockedFuture = atomicOwnerBlocked.get_future();
    std::promise<void> releaseAtomicOwner;
    auto releaseAtomicOwnerFuture =
        releaseAtomicOwner.get_future().share();
    ownerLoop->queueInLoop([&] {
        atomicOwnerBlocked.set_value();
        releaseAtomicOwnerFuture.wait();
    });
    gamenet::test::waitUntilReady(
        atomicOwnerBlockedFuture,
        2s,
        "atomic owner did not enter barrier");

    gamenet::broadcast::BroadcastDispatcher atomicOwnerDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = kConcurrentLimit,
        .maxOutstandingBytesPerOwner = kConcurrentPlans,
        .maxGlobalOutstandingBytes = kConcurrentPlans,
        .lowPriorityOutstandingBytes = kConcurrentPlans,
    });
    std::vector<gamenet::broadcast::BroadcastPlan> ownerPlans;
    ownerPlans.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        ownerPlans.push_back(router.route(
            std::make_shared<const std::string>("x"), targets));
    }
    std::vector<gamenet::broadcast::DispatchSummary> ownerSummaries(
        kConcurrentPlans);
    std::barrier ownerStart(
        static_cast<std::ptrdiff_t>(kConcurrentPlans));
    std::vector<std::thread> ownerProducers;
    ownerProducers.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        ownerProducers.emplace_back([&, index] {
            ownerStart.arrive_and_wait();
            ownerSummaries[index] =
                atomicOwnerDispatcher.dispatch(
                    std::move(ownerPlans[index]));
        });
    }
    for (auto& producer : ownerProducers) {
        producer.join();
    }

    std::size_t ownerAccepted = 0;
    std::size_t ownerRejected = 0;
    for (const auto& summary : ownerSummaries) {
        ownerAccepted += summary.acceptedEndpoints;
        ownerRejected += summary.reasonCount(
            gamenet::broadcast::BroadcastReason::
                OwnerOutstandingTaskLimit);
    }
    GAMENET_TEST_ASSERT(ownerAccepted == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerRejected == kConcurrentPlans - kConcurrentLimit);
    const auto ownerOutstanding =
        atomicOwnerDispatcher.outstanding();
    GAMENET_TEST_ASSERT(
        ownerOutstanding.tasks == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerOutstanding.bytes == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerOutstanding.peakTasks == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerOutstanding.peakBytes == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerOutstanding.rejectedGlobalByteReservations == 0);
    GAMENET_TEST_ASSERT(ownerOutstanding.accepting);
    const auto ownerStats =
        atomicOwnerDispatcher.outstandingStats();
    GAMENET_TEST_ASSERT(ownerStats.owners.size() == 1);
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().ownerId ==
        ownerLoop->executor().id());
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().tasks == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().bytes == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().peakTasks == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().rejectedTaskReservations ==
        kConcurrentPlans - kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        ownerStats.owners.front().rejectedByteReservations == 0);

    const auto ownerSendsBefore = endpoint->sends();
    releaseAtomicOwner.set_value();
    const auto ownerAtomicDeadline =
        std::chrono::steady_clock::now() + 2s;
    while ((atomicOwnerDispatcher.outstanding().tasks != 0 ||
            endpoint->sends() !=
                ownerSendsBefore + kConcurrentLimit) &&
           std::chrono::steady_clock::now() <
               ownerAtomicDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(
        atomicOwnerDispatcher.outstanding().tasks == 0);
    GAMENET_TEST_ASSERT(
        atomicOwnerDispatcher.outstanding().bytes == 0);
    const auto ownerRecoveredStats =
        atomicOwnerDispatcher.outstandingStats();
    GAMENET_TEST_ASSERT(ownerRecoveredStats.owners.size() == 1);
    GAMENET_TEST_ASSERT(
        ownerRecoveredStats.owners.front().tasks == 0);
    GAMENET_TEST_ASSERT(
        ownerRecoveredStats.owners.front().bytes == 0);
    GAMENET_TEST_ASSERT(
        ownerRecoveredStats.owners.front().peakTasks ==
        kConcurrentLimit);

    std::promise<void> firstAtomicGlobalBlocked;
    std::promise<void> secondAtomicGlobalBlocked;
    auto firstAtomicGlobalBlockedFuture =
        firstAtomicGlobalBlocked.get_future();
    auto secondAtomicGlobalBlockedFuture =
        secondAtomicGlobalBlocked.get_future();
    std::promise<void> releaseAtomicGlobal;
    auto releaseAtomicGlobalFuture =
        releaseAtomicGlobal.get_future().share();
    ownerLoop->queueInLoop([&] {
        firstAtomicGlobalBlocked.set_value();
        releaseAtomicGlobalFuture.wait();
    });
    secondOwnerLoop->queueInLoop([&] {
        secondAtomicGlobalBlocked.set_value();
        releaseAtomicGlobalFuture.wait();
    });
    gamenet::test::waitUntilReady(
        firstAtomicGlobalBlockedFuture,
        2s,
        "first atomic global owner did not enter barrier");
    gamenet::test::waitUntilReady(
        secondAtomicGlobalBlockedFuture,
        2s,
        "second atomic global owner did not enter barrier");

    gamenet::broadcast::BroadcastDispatcher atomicGlobalDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = kConcurrentPlans,
        .maxOutstandingBytesPerOwner = kConcurrentPlans,
        .maxGlobalOutstandingBytes = kConcurrentLimit,
        .lowPriorityOutstandingBytes = kConcurrentLimit,
    });
    std::vector<gamenet::broadcast::BroadcastPlan> globalPlans;
    globalPlans.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        globalPlans.push_back(router.route(
            std::make_shared<const std::string>("x"),
            index % 2 == 0 ? targets : secondTargets));
    }
    std::vector<gamenet::broadcast::DispatchSummary> globalSummaries(
        kConcurrentPlans);
    std::barrier globalStart(
        static_cast<std::ptrdiff_t>(kConcurrentPlans));
    std::vector<std::thread> globalProducers;
    globalProducers.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        globalProducers.emplace_back([&, index] {
            globalStart.arrive_and_wait();
            globalSummaries[index] =
                atomicGlobalDispatcher.dispatch(
                    std::move(globalPlans[index]));
        });
    }
    for (auto& producer : globalProducers) {
        producer.join();
    }

    std::size_t globalAccepted = 0;
    std::size_t globalRejected = 0;
    for (const auto& summary : globalSummaries) {
        globalAccepted += summary.acceptedEndpoints;
        globalRejected += summary.reasonCount(
            gamenet::broadcast::BroadcastReason::
                GlobalOutstandingByteLimit);
    }
    GAMENET_TEST_ASSERT(globalAccepted == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        globalRejected == kConcurrentPlans - kConcurrentLimit);
    const auto globalOutstanding =
        atomicGlobalDispatcher.outstanding();
    GAMENET_TEST_ASSERT(
        globalOutstanding.tasks == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        globalOutstanding.bytes == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        globalOutstanding.peakBytes == kConcurrentLimit);
    GAMENET_TEST_ASSERT(
        globalOutstanding.rejectedGlobalByteReservations ==
        kConcurrentPlans - kConcurrentLimit);
    const auto globalStats =
        atomicGlobalDispatcher.outstandingStats();
    GAMENET_TEST_ASSERT(globalStats.owners.size() == 2);
    std::size_t ownerTaskTotal = 0;
    std::size_t ownerByteTotal = 0;
    for (const auto& owner : globalStats.owners) {
        ownerTaskTotal += owner.tasks;
        ownerByteTotal += owner.bytes;
        GAMENET_TEST_ASSERT(
            owner.rejectedTaskReservations == 0);
        GAMENET_TEST_ASSERT(
            owner.rejectedByteReservations == 0);
    }
    GAMENET_TEST_ASSERT(ownerTaskTotal == kConcurrentLimit);
    GAMENET_TEST_ASSERT(ownerByteTotal == kConcurrentLimit);

    const auto globalSendsBefore =
        endpoint->sends() + secondEndpoint->sends();
    releaseAtomicGlobal.set_value();
    const auto globalAtomicDeadline =
        std::chrono::steady_clock::now() + 2s;
    while ((atomicGlobalDispatcher.outstanding().tasks != 0 ||
            endpoint->sends() + secondEndpoint->sends() !=
                globalSendsBefore + kConcurrentLimit) &&
           std::chrono::steady_clock::now() <
               globalAtomicDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(
        atomicGlobalDispatcher.outstanding().tasks == 0);
    GAMENET_TEST_ASSERT(
        atomicGlobalDispatcher.outstanding().bytes == 0);
    for (const auto& owner :
         atomicGlobalDispatcher.outstandingStats().owners) {
        GAMENET_TEST_ASSERT(owner.tasks == 0);
        GAMENET_TEST_ASSERT(owner.bytes == 0);
    }

    std::promise<void> shutdownRaceBlocked;
    auto shutdownRaceBlockedFuture =
        shutdownRaceBlocked.get_future();
    std::promise<void> releaseShutdownRace;
    auto releaseShutdownRaceFuture =
        releaseShutdownRace.get_future().share();
    ownerLoop->queueInLoop([&] {
        shutdownRaceBlocked.set_value();
        releaseShutdownRaceFuture.wait();
    });
    gamenet::test::waitUntilReady(
        shutdownRaceBlockedFuture,
        2s,
        "shutdown-race owner did not enter barrier");

    gamenet::broadcast::BroadcastDispatcher shutdownRaceDispatcher({
        .maxEndpointsPerTask = 1,
        .maxBytesPerTask = 16,
        .maxOutstandingTasksPerOwner = kConcurrentPlans,
        .maxOutstandingBytesPerOwner = kConcurrentPlans,
        .maxGlobalOutstandingBytes = kConcurrentPlans,
        .lowPriorityOutstandingBytes = kConcurrentPlans,
    });
    std::vector<gamenet::broadcast::BroadcastPlan> shutdownRacePlans;
    shutdownRacePlans.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        shutdownRacePlans.push_back(router.route(
            std::make_shared<const std::string>("x"), targets));
    }
    std::vector<gamenet::broadcast::DispatchSummary>
        shutdownRaceSummaries(kConcurrentPlans);
    std::barrier shutdownStart(
        static_cast<std::ptrdiff_t>(kConcurrentPlans + 1));
    std::vector<std::thread> shutdownProducers;
    shutdownProducers.reserve(kConcurrentPlans);
    for (std::size_t index = 0; index < kConcurrentPlans; ++index) {
        shutdownProducers.emplace_back([&, index] {
            shutdownStart.arrive_and_wait();
            shutdownRaceSummaries[index] =
                shutdownRaceDispatcher.dispatch(
                    std::move(shutdownRacePlans[index]));
        });
    }
    std::thread shutdownThread([&] {
        shutdownStart.arrive_and_wait();
        shutdownRaceDispatcher.shutdown();
    });
    for (auto& producer : shutdownProducers) {
        producer.join();
    }
    shutdownThread.join();

    std::size_t shutdownAccepted = 0;
    std::size_t shutdownRejected = 0;
    for (const auto& summary : shutdownRaceSummaries) {
        shutdownAccepted += summary.acceptedEndpoints;
        shutdownRejected += summary.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerShutdown);
        GAMENET_TEST_ASSERT(
            summary.reasonCount(
                gamenet::broadcast::BroadcastReason::
                    OwnerOutstandingTaskLimit) == 0);
        GAMENET_TEST_ASSERT(
            summary.reasonCount(
                gamenet::broadcast::BroadcastReason::
                    GlobalOutstandingByteLimit) == 0);
    }
    GAMENET_TEST_ASSERT(
        shutdownAccepted + shutdownRejected == kConcurrentPlans);
    const auto shutdownRaceOutstanding =
        shutdownRaceDispatcher.outstanding();
    GAMENET_TEST_ASSERT(!shutdownRaceOutstanding.accepting);
    GAMENET_TEST_ASSERT(
        shutdownRaceOutstanding.tasks == shutdownAccepted);
    GAMENET_TEST_ASSERT(
        shutdownRaceOutstanding.bytes == shutdownAccepted);
    GAMENET_TEST_ASSERT(
        shutdownRaceOutstanding.peakTasks <= kConcurrentPlans);
    auto rejectedAfterRace = shutdownRaceDispatcher.dispatch(
        router.route(
            std::make_shared<const std::string>("x"), targets));
    GAMENET_TEST_ASSERT(
        rejectedAfterRace.reasonCount(
            gamenet::broadcast::BroadcastReason::OwnerShutdown) == 1);

    releaseShutdownRace.set_value();
    const auto shutdownRaceDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (shutdownRaceDispatcher.outstanding().tasks != 0 &&
           std::chrono::steady_clock::now() <
               shutdownRaceDeadline) {
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(
        shutdownRaceDispatcher.outstanding().tasks == 0);
    GAMENET_TEST_ASSERT(
        shutdownRaceDispatcher.outstanding().bytes == 0);
    for (const auto& owner :
         shutdownRaceDispatcher.outstandingStats().owners) {
        GAMENET_TEST_ASSERT(owner.tasks == 0);
        GAMENET_TEST_ASSERT(owner.bytes == 0);
    }

    secondOwnerThread.stop();
    ownerThread.stop();
}
