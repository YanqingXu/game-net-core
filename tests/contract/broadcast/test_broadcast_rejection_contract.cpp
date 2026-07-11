#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/game_session/PlayerSession.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {

static_assert(!std::is_default_constructible_v<gamenet::broadcast::BroadcastPlan>);

class ControlledEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    ControlledEndpoint(
        std::uint64_t id,
        gamenet::net::EventLoopExecutor executor,
        gamenet::transport::EndpointResult sendResult =
            gamenet::transport::EndpointResult::Accepted)
        : id_{id}, executor_(std::move(executor)), sendResult_(sendResult) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override { return executor_; }
    gamenet::transport::EndpointResult send(std::string_view) override {
        GAMENET_TEST_ASSERT(executor_.isInOwnerThread());
        sendCalls_.fetch_add(1, std::memory_order_relaxed);
        return sendResult_;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false);
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_.load(); }
    void setOpen(bool open) noexcept { open_.store(open); }
    std::size_t sendCount() const noexcept { return sendCalls_.load(std::memory_order_relaxed); }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoopExecutor executor_;
    gamenet::transport::EndpointResult sendResult_;
    std::atomic<bool> open_{true};
    std::atomic<std::size_t> sendCalls_{0};
};

std::shared_ptr<const gamenet::game_session::PlayerSession> makeSession(
    std::uint64_t id,
    const std::shared_ptr<ControlledEndpoint>& endpoint,
    bool online = true) {
    auto session = std::make_shared<gamenet::game_session::PlayerSession>(
        id,
        "reason-player-" + std::to_string(id),
        endpoint,
        gamenet::game_session::PlayerSession::Clock::now());
    if (online) session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
    return session;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop managementLoop;
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* firstLoop = firstThread.startLoop();
    auto* secondLoop = secondThread.startLoop();

    std::mutex metricsMutex;
    std::vector<gamenet::broadcast::BroadcastMetric> metrics;
    auto collect = [&](const gamenet::broadcast::BroadcastMetric& metric) {
        std::lock_guard lock(metricsMutex);
        metrics.push_back(metric);
    };

    auto first = std::make_shared<ControlledEndpoint>(1, firstLoop->executor());
    auto second = std::make_shared<ControlledEndpoint>(2, secondLoop->executor());
    auto unavailable = std::make_shared<ControlledEndpoint>(3, gamenet::net::EventLoopExecutor{});
    auto closed = std::make_shared<ControlledEndpoint>(4, firstLoop->executor());
    closed->setOpen(false);

    auto onlineFirst = makeSession(1, first);
    auto onlineSecond = makeSession(2, second);
    auto unavailableSession = makeSession(3, unavailable);
    auto closedSession = makeSession(4, closed);
    auto offlineSession = makeSession(5, first, false);
    const auto payload = std::make_shared<const std::string>("four");

    gamenet::broadcast::BroadcastRouter fanoutRouter(
        &managementLoop,
        {.softFanout = 1, .hardFanout = 1, .softBytes = 64, .hardBytes = 64},
        collect);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> fanoutTargets{
        onlineFirst, onlineFirst, offlineSession, onlineSecond};
    (void)fanoutRouter.route(payload, fanoutTargets);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> twoOnline{
        onlineFirst, onlineSecond};
    gamenet::broadcast::BroadcastRouter softRouter(
        &managementLoop,
        {.softFanout = 1, .hardFanout = 4, .softBytes = 64, .hardBytes = 64},
        collect);
    (void)softRouter.route(
        payload, twoOnline, gamenet::broadcast::BroadcastPriority::Low);

    gamenet::broadcast::BroadcastRouter byteRouter(
        &managementLoop,
        {.softFanout = 4, .hardFanout = 4, .softBytes = 3, .hardBytes = 3},
        collect);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> firstOnly{onlineFirst};
    (void)byteRouter.route(payload, firstOnly);

    gamenet::broadcast::BroadcastRouter stateRouter(&managementLoop, {}, collect);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> stateTargets{
        unavailableSession, closedSession};
    (void)stateRouter.route(payload, stateTargets);

    auto oversized = stateRouter.route(payload, firstOnly);
    gamenet::broadcast::BroadcastDispatcher tinyDispatcher(
        {.maxEndpointsPerTask = 1, .maxBytesPerTask = 3}, collect);
    (void)tinyDispatcher.dispatch(std::move(oversized));

    gamenet::broadcast::BroadcastDispatcher validator({}, collect);
    auto movedFrom = stateRouter.route(payload, firstOnly);
    auto validHolder = std::move(movedFrom);
    (void)validHolder;
    (void)validator.dispatch(std::move(movedFrom));

    gamenet::net::EventLoopThread expiringThread;
    auto* expiringLoop = expiringThread.startLoop();
    auto expiring = std::make_shared<ControlledEndpoint>(7, expiringLoop->executor());
    auto expiringSession = makeSession(7, expiring);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> expiringTarget{
        expiringSession};
    auto deadOwner = stateRouter.route(payload, expiringTarget);
    expiringThread.stop();
    (void)validator.dispatch(std::move(deadOwner));

    auto closesAfterRoute = std::make_shared<ControlledEndpoint>(8, firstLoop->executor());
    auto closesAfterRouteSession = makeSession(8, closesAfterRoute);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> closeTarget{
        closesAfterRouteSession};
    auto closePlan = stateRouter.route(payload, closeTarget);
    closesAfterRoute->setOpen(false);
    (void)validator.dispatch(std::move(closePlan));

    std::promise<void> ownerBlockedPromise;
    auto ownerBlockedFuture = ownerBlockedPromise.get_future();
    std::promise<void> releaseOwner;
    auto releaseOwnerFuture = releaseOwner.get_future().share();
    firstLoop->queueInLoop([&] {
        ownerBlockedPromise.set_value();
        releaseOwnerFuture.wait();
    });
    gamenet::test::waitUntilReady(
        ownerBlockedFuture, 2s, "endpoint owner did not enter task-admission barrier");

    auto closesAfterAdmission =
        std::make_shared<ControlledEndpoint>(9, firstLoop->executor());
    auto closesAfterAdmissionSession = makeSession(9, closesAfterAdmission);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> admittedTarget{
        closesAfterAdmissionSession};
    std::promise<void> taskClosedPromise;
    auto taskClosedFuture = taskClosedPromise.get_future();
    std::atomic<bool> taskCloseObserved{false};
    gamenet::broadcast::BroadcastDispatcher admittedDispatcher(
        {},
        [&](const gamenet::broadcast::BroadcastMetric& metric) {
            collect(metric);
            if (metric.transportId.value == 9 &&
                metric.event == gamenet::broadcast::BroadcastMetricEvent::Dropped &&
                metric.reason == gamenet::broadcast::BroadcastReason::EndpointClosed &&
                !taskCloseObserved.exchange(true)) {
                taskClosedPromise.set_value();
            }
        });
    auto admittedPlan = stateRouter.route(payload, admittedTarget);
    const auto admittedSummary = admittedDispatcher.dispatch(std::move(admittedPlan));
    GAMENET_TEST_ASSERT(admittedSummary.scheduledEndpoints == 1);
    GAMENET_TEST_ASSERT(admittedSummary.scheduledTasks == 1);
    closesAfterAdmission->setOpen(false);
    releaseOwner.set_value();
    gamenet::test::waitUntilReady(
        taskClosedFuture, 2s, "close after task admission was not observed");
    GAMENET_TEST_ASSERT(closesAfterAdmission->sendCount() == 0);

    auto rejected = std::make_shared<ControlledEndpoint>(
        6, firstLoop->executor(), gamenet::transport::EndpointResult::WrongThread);
    std::promise<void> rejectionPromise;
    auto rejectionFuture = rejectionPromise.get_future();
    gamenet::broadcast::BroadcastDispatcher rejectionDispatcher(
        {},
        [&](const gamenet::broadcast::BroadcastMetric& metric) {
            collect(metric);
            if (metric.reason == gamenet::broadcast::BroadcastReason::SendRejected) {
                rejectionPromise.set_value();
            }
        });
    auto rejectedSession = makeSession(6, rejected);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> rejectedTarget{
        rejectedSession};
    auto rejectionPlan = stateRouter.route(payload, rejectedTarget);
    const auto rejectionSummary = rejectionDispatcher.dispatch(std::move(rejectionPlan));
    GAMENET_TEST_ASSERT(rejectionSummary.scheduledEndpoints == 1);
    gamenet::test::waitUntilReady(rejectionFuture, 2s, "send rejection was not observed");

    std::unordered_map<gamenet::broadcast::BroadcastReason, std::size_t> reasonCounts;
    std::unordered_map<std::uint64_t, std::size_t> endpointClosedByTransport;
    std::vector<gamenet::broadcast::BroadcastMetric> rejectedMetrics;
    {
        std::lock_guard lock(metricsMutex);
        for (const auto& metric : metrics) {
            if (metric.reason != gamenet::broadcast::BroadcastReason::None) {
                ++reasonCounts[metric.reason];
            }
            if (metric.reason == gamenet::broadcast::BroadcastReason::EndpointClosed) {
                ++endpointClosedByTransport[metric.transportId.value];
            }
            if (metric.transportId.value == 6) rejectedMetrics.push_back(metric);
        }
    }
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::OfflineSession] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::DuplicateEndpoint] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::FanoutHardLimit] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::ByteHardLimit] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::LowPrioritySoftLimit] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::DispatchTaskByteLimit] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::EndpointClosed] == 3);
    GAMENET_TEST_ASSERT(endpointClosedByTransport[4] == 1);
    GAMENET_TEST_ASSERT(endpointClosedByTransport[8] == 1);
    GAMENET_TEST_ASSERT(endpointClosedByTransport[9] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::OwnerUnavailable] == 2);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::InvalidPlan] == 1);
    GAMENET_TEST_ASSERT(reasonCounts[gamenet::broadcast::BroadcastReason::SendRejected] == 1);
    GAMENET_TEST_ASSERT(rejectedMetrics.size() == 3);
    GAMENET_TEST_ASSERT(rejectedMetrics[0].event == gamenet::broadcast::BroadcastMetricEvent::Routed);
    GAMENET_TEST_ASSERT(rejectedMetrics[1].event == gamenet::broadcast::BroadcastMetricEvent::Scheduled);
    GAMENET_TEST_ASSERT(rejectedMetrics[2].event == gamenet::broadcast::BroadcastMetricEvent::Dropped);
    GAMENET_TEST_ASSERT(
        rejectedMetrics[2].reason == gamenet::broadcast::BroadcastReason::SendRejected);

    firstThread.stop();
    secondThread.stop();
}
