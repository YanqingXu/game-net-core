#include "gamenet/game_logic/LogicLoop.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gamenet::game_logic::testing {

using GameCommandQueueHook = void (*)(void*) noexcept;

void setGameCommandQueueTestHooks(
    GameCommandQueueHook beforeSubmitLock,
    GameCommandQueueHook drainLockHeld,
    void* context) noexcept;
void clearGameCommandQueueTestHooks() noexcept;

}  // namespace gamenet::game_logic::testing

namespace {

using gamenet::game_logic::GameCommand;

namespace queue_testing = gamenet::game_logic::testing;

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kCommandsPerProducer = 48;
constexpr std::size_t kCommandsPerTick = 8;
constexpr std::size_t kStopBatchSize = 13;
constexpr std::size_t kCommittedStopCommands = kCommandsPerTick;
constexpr std::size_t kDroppedStopCommands = kStopBatchSize - kCommittedStopCommands;

constexpr std::uint32_t kSeedId = 1;
constexpr std::uint32_t kHandlerReentryId = 2;
constexpr std::uint32_t kOutputReentryId = 3;
constexpr std::uint32_t kMetricReentryId = 4;
constexpr std::uint32_t kProducerBaseId = 1000;
constexpr std::uint32_t kStopBaseId = 100000;
constexpr std::uint32_t kOutputMarkerId = 200000;
constexpr std::uint32_t kCommittedAfterStopOutputMarkerId = 200001;
constexpr std::uint32_t kOutputAfterStopSubmitId = 200002;
constexpr std::uint32_t kPostStopId = 300000;

constexpr std::size_t kNormalHandled =
    1 + kProducerCount * kCommandsPerProducer + 3;
constexpr std::size_t kTotalAccepted = kNormalHandled + kStopBatchSize;
constexpr std::size_t kTotalHandled = kNormalHandled + kCommittedStopCommands;

GameCommand makeCommand(std::uint32_t id, std::string payload) {
    GameCommand command;
    command.messageId = id;
    command.requestId = id;
    command.payload = std::move(payload);
    return command;
}

struct DrainOverlapProbe {
    std::latch drainLockHeld{1};
    std::latch submitEntered{1};
    std::atomic<bool> drainHookObserved{false};
    std::atomic<bool> submitHookObserved{false};
};

void observeSubmitEntry(void* rawProbe) noexcept {
    auto* probe = static_cast<DrainOverlapProbe*>(rawProbe);
    if (probe->submitHookObserved.exchange(true, std::memory_order_acq_rel)) return;
    probe->submitEntered.count_down();
}

void holdDrainUntilSubmitEntry(void* rawProbe) noexcept {
    auto* probe = static_cast<DrainOverlapProbe*>(rawProbe);
    if (probe->drainHookObserved.exchange(true, std::memory_order_acq_rel)) return;
    probe->drainLockHeld.count_down();
    probe->submitEntered.wait();
}

void verifySubmitOverlapsDrainCriticalSection() {
    using namespace std::chrono_literals;
    using gamenet::game_logic::SubmitResult;

    gamenet::net::EventLoop loop;
    gamenet::game_logic::LogicLoop logic(
        &loop,
        {.tickInterval = 1ms,
         .maxCommandsPerTick = 1,
         .queueLimits = {
             .maxCommands = 8,
             .maxQueuedBytes = 64,
             .maxPayloadBytes = 16,
         }});
    std::atomic<std::size_t> handled{0};
    logic.setHandler([&](GameCommand) -> std::optional<GameCommand> {
        handled.fetch_add(1, std::memory_order_release);
        return std::nullopt;
    });
    logic.setMetricCallback([&](const gamenet::game_logic::LogicTickMetric&) {
        if (handled.load(std::memory_order_acquire) != 2) return;
        (void)logic.stop();
        loop.quit();
    });

    GAMENET_TEST_ASSERT(logic.submit(makeCommand(10, "seed")) == SubmitResult::Accepted);
    DrainOverlapProbe probe;
    queue_testing::setGameCommandQueueTestHooks(
        &observeSubmitEntry, &holdDrainUntilSubmitEntry, &probe);

    SubmitResult producerResult = SubmitResult::Stopped;
    std::thread producer([&] {
        probe.drainLockHeld.wait();
        producerResult = logic.submit(makeCommand(11, "overlap"));
    });

    logic.start();
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("submit/drain overlap timed out"); });
    loop.loop();
    producer.join();
    queue_testing::clearGameCommandQueueTestHooks();

    GAMENET_TEST_ASSERT(probe.drainHookObserved.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(probe.submitHookObserved.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(producerResult == SubmitResult::Accepted);
    GAMENET_TEST_ASSERT(handled.load(std::memory_order_acquire) == 2);
}

class LatchReleaseOnExit {
public:
    LatchReleaseOnExit(std::latch* latch, std::atomic<bool>* released)
        : latch_(latch), released_(released) {}

    ~LatchReleaseOnExit() {
        if (!released_->exchange(true, std::memory_order_acq_rel)) latch_->count_down();
    }

private:
    std::latch* latch_;
    std::atomic<bool>* released_;
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using gamenet::game_logic::LogicLoopState;
    using gamenet::game_logic::SubmitResult;

    verifySubmitOverlapsDrainCriticalSection();

    gamenet::net::EventLoop loop;
    gamenet::game_logic::LogicLoop logic(
        &loop,
        {.tickInterval = 1ms,
         .maxCommandsPerTick = kCommandsPerTick,
         .queueLimits = {
             .maxCommands = 2048,
             .maxQueuedBytes = 128U * 1024U,
             .maxPayloadBytes = 64,
         }});

    std::latch producersMayStart{1};
    std::latch firstProducerSubmissions{kProducerCount};
    std::atomic<bool> producerGateReleased{false};
    std::atomic<bool> handlerActive{false};
    std::atomic<std::size_t> producerOverlapCount{0};
    std::atomic<std::size_t> producerAccepted{0};
    std::atomic<std::size_t> producersRemaining{kProducerCount};
    std::atomic<bool> producerFailure{false};

    std::unordered_set<std::uint32_t> handledIds;
    std::vector<std::uint32_t> handledOrder;
    handledIds.reserve(kTotalAccepted);
    handledOrder.reserve(kTotalHandled);
    bool duplicateObserved = false;
    bool handlerReentryAccepted = false;
    bool outputReentryAccepted = false;
    bool metricReentryAccepted = false;
    bool metricReentryAttempted = false;
    bool stopBatchQueued = false;
    bool stopIssuedInHandler = false;
    bool timedOut = false;
    std::size_t outputCallbacks = 0;
    std::size_t metricCallbacks = 0;
    SubmitResult outputAfterStopSubmit = SubmitResult::Accepted;
    SubmitResult postStopSubmit = SubmitResult::Accepted;
    std::optional<gamenet::game_logic::LogicStopSummary> callbackStopSummary;
    std::optional<gamenet::game_logic::LogicTickMetric> finalTickMetric;

    logic.setHandler([&](GameCommand command) -> std::optional<GameCommand> {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        const auto id = command.messageId;
        duplicateObserved |= !handledIds.insert(id).second;
        handledOrder.push_back(id);

        if (id == kSeedId) {
            handlerActive.store(true, std::memory_order_release);
            if (!producerGateReleased.exchange(true, std::memory_order_acq_rel)) {
                producersMayStart.count_down();
            }
            handlerReentryAccepted =
                logic.submit(makeCommand(kHandlerReentryId, "handler-reentry")) ==
                SubmitResult::Accepted;

            // Every producer must complete a real submit while this handler is
            // still executing. If the queue mutex leaked into callbacks this
            // explicit latch would deadlock and the CTest timeout would fail.
            firstProducerSubmissions.wait();
            handlerActive.store(false, std::memory_order_release);
            return makeCommand(kOutputMarkerId, "output-marker");
        }

        if (id == kStopBaseId) {
            stopIssuedInHandler = true;
            callbackStopSummary = logic.stop();
        } else if (id == kStopBaseId + 1) {
            return makeCommand(kCommittedAfterStopOutputMarkerId, "committed-output");
        }
        return std::nullopt;
    });

    logic.setOutputCallback([&](GameCommand output) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++outputCallbacks;
        if (output.messageId == kOutputMarkerId) {
            outputReentryAccepted =
                logic.submit(makeCommand(kOutputReentryId, "output-reentry")) ==
                SubmitResult::Accepted;
        } else {
            GAMENET_TEST_ASSERT(output.messageId == kCommittedAfterStopOutputMarkerId);
            outputAfterStopSubmit =
                logic.submit(makeCommand(kOutputAfterStopSubmitId, "output-after-stop"));
        }
    });

    logic.setMetricCallback([&](const gamenet::game_logic::LogicTickMetric& metric) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++metricCallbacks;

        if (!metricReentryAttempted) {
            metricReentryAttempted = true;
            metricReentryAccepted =
                logic.submit(makeCommand(kMetricReentryId, "metric-reentry")) ==
                SubmitResult::Accepted;
        }

        const bool producersDone =
            producersRemaining.load(std::memory_order_acquire) == 0;
        if (producersDone && producerFailure.load(std::memory_order_acquire)) {
            (void)logic.stop();
            loop.quit();
            return;
        }

        if (!stopBatchQueued && producersDone &&
            producerAccepted.load(std::memory_order_acquire) ==
                kProducerCount * kCommandsPerProducer &&
            handledIds.size() == kNormalHandled && metric.queue.depth == 0) {
            stopBatchQueued = true;
            for (std::size_t index = 0; index < kStopBatchSize; ++index) {
                GAMENET_TEST_ASSERT(
                    logic.submit(makeCommand(
                        kStopBaseId + static_cast<std::uint32_t>(index), "stop")) ==
                    SubmitResult::Accepted);
            }
            return;
        }

        if (stopIssuedInHandler) {
            GAMENET_TEST_ASSERT(logic.state() == LogicLoopState::Stopped);
            finalTickMetric = metric;
            postStopSubmit = logic.submit(makeCommand(kPostStopId, "post-stop"));
            loop.quit();
        }
    });

    logic.start();
    GAMENET_TEST_ASSERT(logic.submit(makeCommand(kSeedId, "seed")) == SubmitResult::Accepted);

    std::vector<std::jthread> producers;
    LatchReleaseOnExit releaseProducerGateOnExit(&producersMayStart, &producerGateReleased);
    producers.reserve(kProducerCount);
    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            producersMayStart.wait();
            for (std::size_t index = 0; index < kCommandsPerProducer; ++index) {
                const auto id = kProducerBaseId + static_cast<std::uint32_t>(
                    producer * kCommandsPerProducer + index);
                const auto result = logic.submit(makeCommand(id, "producer"));
                if (result == SubmitResult::Accepted) {
                    producerAccepted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    producerFailure.store(true, std::memory_order_release);
                }
                if (index == 0) {
                    if (handlerActive.load(std::memory_order_acquire)) {
                        producerOverlapCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    firstProducerSubmissions.count_down();
                }
            }
            producersRemaining.fetch_sub(1, std::memory_order_release);
        });
    }

    const auto timeout = loop.runAfter(5s, [&] {
        timedOut = true;
        if (logic.running()) (void)logic.stop();
        loop.quit();
    });
    loop.loop();
    for (auto& producer : producers) producer.join();
    loop.cancel(timeout);

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(!producerFailure.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(
        producerAccepted.load(std::memory_order_acquire) ==
        kProducerCount * kCommandsPerProducer);
    GAMENET_TEST_ASSERT(producerOverlapCount.load(std::memory_order_acquire) == kProducerCount);
    GAMENET_TEST_ASSERT(handlerReentryAccepted);
    GAMENET_TEST_ASSERT(outputReentryAccepted);
    GAMENET_TEST_ASSERT(metricReentryAccepted);
    GAMENET_TEST_ASSERT(outputCallbacks == 2);
    GAMENET_TEST_ASSERT(metricCallbacks > 1);
    GAMENET_TEST_ASSERT(!duplicateObserved);

    GAMENET_TEST_ASSERT(stopBatchQueued);
    GAMENET_TEST_ASSERT(stopIssuedInHandler);
    GAMENET_TEST_ASSERT(callbackStopSummary.has_value());
    GAMENET_TEST_ASSERT(callbackStopSummary->droppedCommands == kDroppedStopCommands);
    GAMENET_TEST_ASSERT(callbackStopSummary->droppedBytes == kDroppedStopCommands * 4);
    GAMENET_TEST_ASSERT(outputAfterStopSubmit == SubmitResult::Stopped);
    GAMENET_TEST_ASSERT(postStopSubmit == SubmitResult::Stopped);
    GAMENET_TEST_ASSERT(logic.state() == LogicLoopState::Stopped);

    GAMENET_TEST_ASSERT(handledIds.size() == kTotalHandled);
    GAMENET_TEST_ASSERT(handledOrder.size() == kTotalHandled);
    GAMENET_TEST_ASSERT(handledIds.contains(kSeedId));
    GAMENET_TEST_ASSERT(handledIds.contains(kHandlerReentryId));
    GAMENET_TEST_ASSERT(handledIds.contains(kOutputReentryId));
    GAMENET_TEST_ASSERT(handledIds.contains(kMetricReentryId));
    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
        for (std::size_t index = 0; index < kCommandsPerProducer; ++index) {
            const auto id = kProducerBaseId + static_cast<std::uint32_t>(
                producer * kCommandsPerProducer + index);
            GAMENET_TEST_ASSERT(handledIds.contains(id));
        }
    }
    for (std::size_t index = 0; index < kCommittedStopCommands; ++index) {
        GAMENET_TEST_ASSERT(
            handledIds.contains(kStopBaseId + static_cast<std::uint32_t>(index)));
    }
    for (std::size_t index = kCommittedStopCommands; index < kStopBatchSize; ++index) {
        GAMENET_TEST_ASSERT(
            !handledIds.contains(kStopBaseId + static_cast<std::uint32_t>(index)));
    }
    const auto stopBatchOffset = handledOrder.size() - kCommittedStopCommands;
    for (std::size_t index = 0; index < kCommittedStopCommands; ++index) {
        GAMENET_TEST_ASSERT(
            handledOrder[stopBatchOffset + index] ==
            kStopBaseId + static_cast<std::uint32_t>(index));
    }

    GAMENET_TEST_ASSERT(finalTickMetric.has_value());
    GAMENET_TEST_ASSERT(finalTickMetric->drained == kCommittedStopCommands);
    GAMENET_TEST_ASSERT(finalTickMetric->queue.depth == 0);
    GAMENET_TEST_ASSERT(!finalTickMetric->queue.accepting);
    GAMENET_TEST_ASSERT(finalTickMetric->queue.droppedOnStop == kDroppedStopCommands);
    GAMENET_TEST_ASSERT(finalTickMetric->queue.droppedBytesOnStop == kDroppedStopCommands * 4);
    GAMENET_TEST_ASSERT(finalTickMetric->queue.rejectedStopped == 1);

    const auto snapshot = logic.queueSnapshot();
    GAMENET_TEST_ASSERT(snapshot.accepted == kTotalAccepted);
    GAMENET_TEST_ASSERT(snapshot.accepted == handledIds.size() + snapshot.droppedOnStop);
    GAMENET_TEST_ASSERT(snapshot.rejectedFull == 0);
    GAMENET_TEST_ASSERT(snapshot.rejectedPayload == 0);
    GAMENET_TEST_ASSERT(snapshot.rejectedStopped == 2);
    GAMENET_TEST_ASSERT(snapshot.droppedOnStop == kDroppedStopCommands);
    GAMENET_TEST_ASSERT(snapshot.droppedBytesOnStop == kDroppedStopCommands * 4);
    const auto repeatedStop = logic.stop();
    GAMENET_TEST_ASSERT(repeatedStop.droppedCommands == 0);
    GAMENET_TEST_ASSERT(repeatedStop.droppedBytes == 0);
}
