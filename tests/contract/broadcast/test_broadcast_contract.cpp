#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/game_session/PlayerSession.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

class RecordingEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    RecordingEndpoint(std::uint64_t id, gamenet::net::EventLoop* loop) : id_{id}, loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        GAMENET_TEST_ASSERT(loop_->isInLoopThread());
        std::lock_guard lock(mutex_);
        payloads_.emplace_back(bytes);
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false);
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_.load(); }
    std::size_t sendCount() const {
        std::lock_guard lock(mutex_);
        return payloads_.size();
    }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    std::atomic<bool> open_{true};
    mutable std::mutex mutex_;
    std::vector<std::string> payloads_;
};

std::shared_ptr<const gamenet::game_session::PlayerSession> makeSession(
    std::uint64_t id,
    const std::shared_ptr<RecordingEndpoint>& endpoint,
    bool online = true) {
    auto session = std::make_shared<gamenet::game_session::PlayerSession>(
        id, "player-" + std::to_string(id), endpoint,
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

    auto e1 = std::make_shared<RecordingEndpoint>(1, firstLoop);
    auto e2 = std::make_shared<RecordingEndpoint>(2, secondLoop);
    auto e3 = std::make_shared<RecordingEndpoint>(3, firstLoop);
    auto e4 = std::make_shared<RecordingEndpoint>(4, secondLoop);
    auto s1 = makeSession(1, e1);
    auto s2 = makeSession(2, e2);
    auto s3 = makeSession(3, e3);
    auto s4 = makeSession(4, e4);
    auto offline = makeSession(5, std::make_shared<RecordingEndpoint>(5, firstLoop), false);

    std::mutex metricsMutex;
    std::vector<gamenet::broadcast::BroadcastMetric> metrics;
    auto metricCallback = [&](const auto& metric) {
        std::lock_guard lock(metricsMutex);
        metrics.push_back(metric);
    };

    gamenet::broadcast::BroadcastRouter router(
        &managementLoop,
        {.softFanout = 2, .hardFanout = 3, .softBytes = 8, .hardBytes = 12},
        metricCallback);
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> targets{
        s1, s1, offline, s2, s3, s4};
    auto payload = std::make_shared<const std::string>("data");
    auto plan = router.route(payload, targets, gamenet::broadcast::BroadcastPriority::Normal);
    GAMENET_TEST_ASSERT(plan.payload() == payload);
    GAMENET_TEST_ASSERT(plan.accepted() == 3);
    GAMENET_TEST_ASSERT(plan.dropped() == 3);
    GAMENET_TEST_ASSERT(plan.batchCount() == 2);

    std::promise<void> sentPromise;
    auto sentFuture = sentPromise.get_future();
    std::atomic<int> sentMetrics{0};
    auto dispatchMetrics = [&](const gamenet::broadcast::BroadcastMetric& metric) {
        metricCallback(metric);
        if (metric.event == gamenet::broadcast::BroadcastMetricEvent::Sent &&
            sentMetrics.fetch_add(1) + 1 == 3) {
            sentPromise.set_value();
        }
    };
    gamenet::broadcast::BroadcastDispatcher dispatcher(
        {.maxEndpointsPerTask = 1, .maxBytesPerTask = 4}, dispatchMetrics);
    auto summary = dispatcher.dispatch(std::move(plan));
    GAMENET_TEST_ASSERT(summary.scheduledEndpoints == 3);
    GAMENET_TEST_ASSERT(summary.scheduledTasks == 3);
    gamenet::test::waitUntilReady(sentFuture, 2s, "broadcast sends did not finish");
    GAMENET_TEST_ASSERT(e1->sendCount() == 1);
    GAMENET_TEST_ASSERT(e2->sendCount() == 1);
    GAMENET_TEST_ASSERT(e3->sendCount() == 1);
    GAMENET_TEST_ASSERT(e4->sendCount() == 0);

    auto lowPlan = router.route(payload, std::span(targets).subspan(3),
                                gamenet::broadcast::BroadcastPriority::Low);
    GAMENET_TEST_ASSERT(lowPlan.accepted() == 2);
    GAMENET_TEST_ASSERT(lowPlan.dropped() == 1);

    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> oneTarget{s4};
    auto oversizedPlan = router.route(payload, oneTarget);
    gamenet::broadcast::BroadcastDispatcher tinyDispatcher(
        {.maxEndpointsPerTask = 1, .maxBytesPerTask = 3}, metricCallback);
    const auto oversizedSummary = tinyDispatcher.dispatch(std::move(oversizedPlan));
    GAMENET_TEST_ASSERT(oversizedSummary.scheduledEndpoints == 0);
    GAMENET_TEST_ASSERT(oversizedSummary.scheduledTasks == 0);

    bool sawDuplicate = false;
    bool sawOffline = false;
    bool sawHardLimit = false;
    bool sawSoftLimit = false;
    bool sawTaskByteLimit = false;
    {
        std::lock_guard lock(metricsMutex);
        for (const auto& metric : metrics) {
            sawDuplicate |= metric.reason == gamenet::broadcast::BroadcastReason::DuplicateEndpoint;
            sawOffline |= metric.reason == gamenet::broadcast::BroadcastReason::OfflineSession;
            sawHardLimit |= metric.reason == gamenet::broadcast::BroadcastReason::FanoutHardLimit ||
                            metric.reason == gamenet::broadcast::BroadcastReason::ByteHardLimit;
            sawSoftLimit |= metric.reason == gamenet::broadcast::BroadcastReason::LowPrioritySoftLimit;
            sawTaskByteLimit |=
                metric.reason == gamenet::broadcast::BroadcastReason::DispatchTaskByteLimit;
        }
    }
    GAMENET_TEST_ASSERT(
        sawDuplicate && sawOffline && sawHardLimit && sawSoftLimit && sawTaskByteLimit);

    firstThread.stop();
    secondThread.stop();
}
