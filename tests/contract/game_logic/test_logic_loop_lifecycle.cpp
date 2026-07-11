#include "gamenet/game_logic/LogicLoop.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

int main() {
    using namespace std::chrono_literals;
    using gamenet::game_logic::GameCommand;
    using gamenet::game_logic::LogicLoopState;
    using gamenet::game_logic::SubmitResult;

    gamenet::net::EventLoop loop;
    {
        gamenet::game_logic::LogicLoop missingHandler(&loop);
        bool rejectedMissingHandler = false;
        try {
            missingHandler.start();
        } catch (const std::logic_error&) {
            rejectedMissingHandler = true;
        }
        GAMENET_TEST_ASSERT(rejectedMissingHandler);
        GAMENET_TEST_ASSERT(missingHandler.state() == LogicLoopState::Created);
    }

    gamenet::game_logic::LogicLoop logic(
        &loop,
        {.tickInterval = 10ms,
         .maxCommandsPerTick = 1,
         .queueLimits = {.maxCommands = 8, .maxQueuedBytes = 64, .maxPayloadBytes = 16}});
    GAMENET_TEST_ASSERT(logic.state() == LogicLoopState::Created);
    GAMENET_TEST_ASSERT(!logic.running());

    logic.setHandler([](GameCommand) -> std::optional<GameCommand> { return std::nullopt; });
    logic.start();
    logic.start();
    GAMENET_TEST_ASSERT(logic.state() == LogicLoopState::Running);
    GAMENET_TEST_ASSERT(logic.running());

    GameCommand first;
    first.payload = "abc";
    GameCommand second;
    second.payload = "12345";
    GAMENET_TEST_ASSERT(logic.submit(std::move(first)) == SubmitResult::Accepted);
    GAMENET_TEST_ASSERT(logic.submit(std::move(second)) == SubmitResult::Accepted);

    const auto stopped = logic.stop();
    GAMENET_TEST_ASSERT(stopped.droppedCommands == 2);
    GAMENET_TEST_ASSERT(stopped.droppedBytes == 8);
    GAMENET_TEST_ASSERT(logic.state() == LogicLoopState::Stopped);
    GAMENET_TEST_ASSERT(!logic.running());
    GAMENET_TEST_ASSERT(logic.queueSnapshot().depth == 0);
    GAMENET_TEST_ASSERT(logic.queueSnapshot().droppedOnStop == 2);
    GAMENET_TEST_ASSERT(logic.queueSnapshot().droppedBytesOnStop == 8);

    const auto repeatedStop = logic.stop();
    GAMENET_TEST_ASSERT(repeatedStop.droppedCommands == 0);
    GAMENET_TEST_ASSERT(repeatedStop.droppedBytes == 0);

    bool restartRejected = false;
    try {
        logic.start();
    } catch (const std::logic_error&) {
        restartRejected = true;
    }
    GAMENET_TEST_ASSERT(restartRejected);

    GameCommand rejected;
    rejected.payload = "x";
    GAMENET_TEST_ASSERT(logic.submit(std::move(rejected)) == SubmitResult::Stopped);

    bool destroyedTimerRan = false;
    {
        auto pendingTimer = std::make_unique<gamenet::game_logic::LogicLoop>(
            &loop,
            gamenet::game_logic::LogicLoopOptions{
                .tickInterval = 1ms,
                .maxCommandsPerTick = 1,
            });
        pendingTimer->setHandler([&](GameCommand) -> std::optional<GameCommand> {
            destroyedTimerRan = true;
            return std::nullopt;
        });
        pendingTimer->start();
    }

    int callbackDestructions = 0;
    bool callbackDestructionTimedOut = false;
    auto recordCallbackDestruction = [&] {
        ++callbackDestructions;
        if (callbackDestructions == 3) loop.quit();
    };
    auto command = [](std::uint64_t id) {
        GameCommand value;
        value.requestId = id;
        value.payload = "destroy";
        return value;
    };

    std::unique_ptr<gamenet::game_logic::LogicLoop> handlerOwned;
    handlerOwned = std::make_unique<gamenet::game_logic::LogicLoop>(
        &loop,
        gamenet::game_logic::LogicLoopOptions{.tickInterval = 1ms, .maxCommandsPerTick = 1});
    handlerOwned->setHandler([&](GameCommand value) -> std::optional<GameCommand> {
        handlerOwned.reset();
        recordCallbackDestruction();
        return value;
    });
    GAMENET_TEST_ASSERT(handlerOwned->submit(command(10)) == SubmitResult::Accepted);
    handlerOwned->start();

    std::unique_ptr<gamenet::game_logic::LogicLoop> outputOwned;
    outputOwned = std::make_unique<gamenet::game_logic::LogicLoop>(
        &loop,
        gamenet::game_logic::LogicLoopOptions{.tickInterval = 1ms, .maxCommandsPerTick = 1});
    outputOwned->setHandler(
        [](GameCommand value) -> std::optional<GameCommand> { return value; });
    outputOwned->setOutputCallback([&](GameCommand) {
        outputOwned.reset();
        recordCallbackDestruction();
    });
    GAMENET_TEST_ASSERT(outputOwned->submit(command(20)) == SubmitResult::Accepted);
    outputOwned->start();

    std::unique_ptr<gamenet::game_logic::LogicLoop> metricOwned;
    metricOwned = std::make_unique<gamenet::game_logic::LogicLoop>(
        &loop,
        gamenet::game_logic::LogicLoopOptions{.tickInterval = 1ms, .maxCommandsPerTick = 1});
    metricOwned->setHandler(
        [](GameCommand) -> std::optional<GameCommand> { return std::nullopt; });
    metricOwned->setMetricCallback([&](const gamenet::game_logic::LogicTickMetric&) {
        metricOwned.reset();
        recordCallbackDestruction();
    });
    GAMENET_TEST_ASSERT(metricOwned->submit(command(30)) == SubmitResult::Accepted);
    metricOwned->start();

    loop.runAfter(1s, [&] {
        callbackDestructionTimedOut = true;
        loop.quit();
    });
    loop.loop();
    GAMENET_TEST_ASSERT(!destroyedTimerRan);
    GAMENET_TEST_ASSERT(!callbackDestructionTimedOut);
    GAMENET_TEST_ASSERT(callbackDestructions == 3);
    GAMENET_TEST_ASSERT(!handlerOwned && !outputOwned && !metricOwned);
}
