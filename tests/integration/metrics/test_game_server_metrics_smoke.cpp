#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastMetricsRecorder.h"
#include "gamenet/broadcast/BroadcastRouter.h"
#include "gamenet/core/metrics/MetricsExporter.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/game_logic/LogicMetricsRecorder.h"
#include "gamenet/game_session/PlayerSession.h"

#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ThrowingExporter final : public gamenet::metrics::MetricsExporter {
public:
    void incrementCounter(std::string_view, std::uint64_t) override {
        throw std::runtime_error("counter sink failed");
    }
    void observeHistogram(std::string_view, double) override {
        throw std::runtime_error("histogram sink failed");
    }
};

class RecordingEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    RecordingEndpoint(gamenet::transport::TransportSessionId id, gamenet::net::EventLoop* loop)
        : id_(id), loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        GAMENET_TEST_ASSERT(loop_->isInLoopThread());
        sends_.fetch_add(1);
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return true; }
    int sends() const noexcept { return sends_.load(); }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    std::atomic<int> sends_{0};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    auto sink = std::make_shared<gamenet::metrics::InMemoryMetricsExporter>();
    const gamenet::metrics::MetricLabels labels{{"service", "game-server"}, {"zone", "test"}};
    auto tagged = std::make_shared<gamenet::metrics::TaggedMetricsExporter>(sink, labels);

    auto endpoint = std::make_shared<RecordingEndpoint>(
        gamenet::transport::TransportSessionId{42},
        &loop);
    auto session = std::make_shared<gamenet::game_session::PlayerSession>(
        7,
        "player-7",
        endpoint,
        gamenet::game_session::PlayerSession::Clock::now());
    session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> targets{session};

    auto broadcastMetrics = gamenet::broadcast::makeBroadcastMetricsCallback(tagged);
    gamenet::broadcast::BroadcastRouter router(&loop, {}, broadcastMetrics);
    gamenet::broadcast::BroadcastDispatcher dispatcher({}, broadcastMetrics);
    auto plan = router.route(std::make_shared<const std::string>("payload"), targets);
    const auto dispatched = dispatcher.dispatch(std::move(plan));
    GAMENET_TEST_ASSERT(dispatched.scheduledEndpoints == 1);

    gamenet::game_logic::LogicLoop logic(
        &loop,
        {.tickInterval = 1ms, .maxCommandsPerTick = 4, .queueLimits = {}});
    logic.setMetricCallback(gamenet::game_logic::makeLogicMetricsCallback(tagged));
    logic.setHandler([](gamenet::game_logic::GameCommand) {
        return std::optional<gamenet::game_logic::GameCommand>{};
    });
    gamenet::game_logic::GameCommand command;
    command.sessionId = 7;
    command.transportId = gamenet::transport::TransportSessionId{42};
    command.payload = "move";
    GAMENET_TEST_ASSERT(logic.submit(std::move(command)) == gamenet::game_logic::SubmitResult::Accepted);
    logic.start();

    bool timedOut = false;
    loop.runEvery(1ms, [&] {
        const auto logicCounter = gamenet::metrics::metricNameWithLabels(
            "gamenet.game_logic.tick_completed", labels);
        if (endpoint->sends() == 1 && sink->counterValue(logicCounter) != 0) {
            logic.stop();
            loop.quit();
        }
    });
    loop.runAfter(2s, [&] {
        timedOut = true;
        logic.stop();
        loop.quit();
    });
    loop.loop();
    GAMENET_TEST_ASSERT(!timedOut);

    GAMENET_TEST_ASSERT(sink->counterValue(gamenet::metrics::metricNameWithLabels(
        "gamenet.broadcast.event.routed", labels)) == 1);
    GAMENET_TEST_ASSERT(sink->counterValue(gamenet::metrics::metricNameWithLabels(
        "gamenet.broadcast.event.sent", labels)) == 1);
    GAMENET_TEST_ASSERT(sink->histogram(gamenet::metrics::metricNameWithLabels(
        "gamenet.broadcast.payload_bytes", labels)).max == 7);
    GAMENET_TEST_ASSERT(sink->histogram(gamenet::metrics::metricNameWithLabels(
        "gamenet.game_logic.commands_drained", labels)).max == 1);

    const auto text = gamenet::metrics::renderPrometheusText(sink->snapshot());
    GAMENET_TEST_ASSERT(text.find(
        "gamenet_broadcast_event_sent{service=\"game-server\",zone=\"test\"} 1") !=
        std::string::npos);
    GAMENET_TEST_ASSERT(text.find("gamenet_game_logic_commands_drained_count") != std::string::npos);

    auto throwing = std::make_shared<ThrowingExporter>();
    gamenet::game_logic::makeLogicMetricsCallback(throwing)({});
    gamenet::broadcast::makeBroadcastMetricsCallback(throwing)({});
}
