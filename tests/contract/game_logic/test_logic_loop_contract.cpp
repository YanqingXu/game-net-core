#include "gamenet/game_logic/LogicLoop.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop eventLoop;
    gamenet::game_logic::LogicLoop logicLoop(
        &eventLoop,
        {.tickInterval = 1ms,
         .maxCommandsPerTick = 2,
         .queueLimits = {.maxCommands = 3, .maxQueuedBytes = 12, .maxPayloadBytes = 6}});

    std::vector<std::string> outputs;
    std::vector<gamenet::game_logic::LogicTickMetric> metrics;
    logicLoop.setHandler([&](gamenet::game_logic::GameCommand command)
                             -> std::optional<gamenet::game_logic::GameCommand> {
        GAMENET_TEST_ASSERT(eventLoop.isInLoopThread());
        command.payload = "ok:" + command.payload;
        return command;
    });
    logicLoop.setOutputCallback([&](gamenet::game_logic::GameCommand command) {
        GAMENET_TEST_ASSERT(eventLoop.isInLoopThread());
        outputs.push_back(std::move(command.payload));
        if (outputs.size() == 3) {
            logicLoop.stop();
            eventLoop.quit();
        }
    });
    logicLoop.setMetricCallback([&](const auto& metric) {
        GAMENET_TEST_ASSERT(eventLoop.isInLoopThread());
        GAMENET_TEST_ASSERT(metric.drained <= 2);
        metrics.push_back(metric);
    });
    logicLoop.start();

    std::vector<gamenet::game_logic::SubmitResult> results;
    std::thread ioThread([&] {
        for (const auto* payload : {"a", "bb", "ccc", "fourth"}) {
            gamenet::game_logic::GameCommand command;
            command.payload = payload;
            results.push_back(logicLoop.submit(std::move(command)));
        }
        gamenet::game_logic::GameCommand oversized;
        oversized.payload = "1234567";
        results.push_back(logicLoop.submit(std::move(oversized)));
    });
    ioThread.join();

    eventLoop.runAfter(2s, [&] { GAMENET_TEST_FAIL("logic loop contract timed out"); });
    eventLoop.loop();

    GAMENET_TEST_ASSERT(results.size() == 5);
    GAMENET_TEST_ASSERT(results[0] == gamenet::game_logic::SubmitResult::Accepted);
    GAMENET_TEST_ASSERT(results[1] == gamenet::game_logic::SubmitResult::Accepted);
    GAMENET_TEST_ASSERT(results[2] == gamenet::game_logic::SubmitResult::Accepted);
    GAMENET_TEST_ASSERT(results[3] == gamenet::game_logic::SubmitResult::QueueFull);
    GAMENET_TEST_ASSERT(results[4] == gamenet::game_logic::SubmitResult::PayloadTooLarge);
    GAMENET_TEST_ASSERT((outputs == std::vector<std::string>{"ok:a", "ok:bb", "ok:ccc"}));
    GAMENET_TEST_ASSERT(metrics.size() >= 2);

    gamenet::game_logic::GameCommand stopped;
    stopped.payload = "x";
    GAMENET_TEST_ASSERT(logicLoop.submit(std::move(stopped)) == gamenet::game_logic::SubmitResult::Stopped);
    const auto snapshot = logicLoop.queueSnapshot();
    GAMENET_TEST_ASSERT(snapshot.accepted == 3);
    GAMENET_TEST_ASSERT(snapshot.rejectedFull == 1);
    GAMENET_TEST_ASSERT(snapshot.rejectedPayload == 1);
    GAMENET_TEST_ASSERT(snapshot.rejectedStopped == 1);
    GAMENET_TEST_ASSERT(snapshot.depthHighWatermark == 3);
    GAMENET_TEST_ASSERT(!snapshot.accepting);
}
