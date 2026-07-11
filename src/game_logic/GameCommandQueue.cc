#include "gamenet/game_logic/GameCommandQueue.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace gamenet::game_logic {

#if defined(GAMENET_ENABLE_INTERNAL_TEST_HOOKS)
namespace testing {

using GameCommandQueueHook = void (*)(void*) noexcept;

namespace {

struct GameCommandQueueTestHooks {
    std::atomic<GameCommandQueueHook> beforeSubmitLock{nullptr};
    std::atomic<GameCommandQueueHook> drainLockHeld{nullptr};
    std::atomic<void*> context{nullptr};
};

GameCommandQueueTestHooks testHooks;

void invokeHook(const std::atomic<GameCommandQueueHook>& hookSlot) noexcept {
    const auto hook = hookSlot.load(std::memory_order_acquire);
    if (!hook) return;
    hook(testHooks.context.load(std::memory_order_acquire));
}

}  // namespace

void setGameCommandQueueTestHooks(
    GameCommandQueueHook beforeSubmitLock,
    GameCommandQueueHook drainLockHeld,
    void* context) noexcept {
    testHooks.context.store(context, std::memory_order_release);
    testHooks.beforeSubmitLock.store(beforeSubmitLock, std::memory_order_release);
    testHooks.drainLockHeld.store(drainLockHeld, std::memory_order_release);
}

void clearGameCommandQueueTestHooks() noexcept {
    testHooks.beforeSubmitLock.store(nullptr, std::memory_order_release);
    testHooks.drainLockHeld.store(nullptr, std::memory_order_release);
    testHooks.context.store(nullptr, std::memory_order_release);
}

}  // namespace testing
#endif

GameCommandQueue::GameCommandQueue(QueueLimits limits) : limits_(limits) {
    if (limits_.maxCommands == 0 || limits_.maxQueuedBytes == 0 ||
        limits_.maxPayloadBytes == 0 || limits_.maxPayloadBytes > limits_.maxQueuedBytes) {
        throw std::invalid_argument("GameCommandQueue requires coherent non-zero limits");
    }
}

SubmitResult GameCommandQueue::submit(GameCommand command) {
    const auto bytes = command.payload.size();
#if defined(GAMENET_ENABLE_INTERNAL_TEST_HOOKS)
    testing::invokeHook(testing::testHooks.beforeSubmitLock);
#endif
    std::lock_guard lock(mutex_);
    if (!accepting_) {
        ++rejectedStopped_;
        return SubmitResult::Stopped;
    }
    if (bytes > limits_.maxPayloadBytes) {
        ++rejectedPayload_;
        return SubmitResult::PayloadTooLarge;
    }
    if (queue_.size() >= limits_.maxCommands || bytes > limits_.maxQueuedBytes - queuedBytes_) {
        ++rejectedFull_;
        return SubmitResult::QueueFull;
    }
    if (command.enqueuedAt == std::chrono::steady_clock::time_point{}) {
        command.enqueuedAt = std::chrono::steady_clock::now();
    }
    queuedBytes_ += bytes;
    queue_.push_back(std::move(command));
    ++accepted_;
    depthHighWatermark_ = std::max(depthHighWatermark_, queue_.size());
    bytesHighWatermark_ = std::max(bytesHighWatermark_, queuedBytes_);
    return SubmitResult::Accepted;
}

std::vector<GameCommand> GameCommandQueue::drain(std::size_t maxCommands) {
    std::vector<GameCommand> result;
    std::lock_guard lock(mutex_);
#if defined(GAMENET_ENABLE_INTERNAL_TEST_HOOKS)
    testing::invokeHook(testing::testHooks.drainLockHeld);
#endif
    const auto count = std::min(maxCommands, queue_.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        queuedBytes_ -= queue_.front().payload.size();
        result.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return result;
}

QueueSnapshot GameCommandQueue::snapshot() const {
    std::lock_guard lock(mutex_);
    return {
        .depth = queue_.size(),
        .queuedBytes = queuedBytes_,
        .depthHighWatermark = depthHighWatermark_,
        .bytesHighWatermark = bytesHighWatermark_,
        .accepted = accepted_,
        .rejectedFull = rejectedFull_,
        .rejectedPayload = rejectedPayload_,
        .rejectedStopped = rejectedStopped_,
        .droppedOnStop = droppedOnStop_,
        .droppedBytesOnStop = droppedBytesOnStop_,
        .accepting = accepting_,
    };
}

void GameCommandQueue::close() {
    std::lock_guard lock(mutex_);
    accepting_ = false;
}

QueueDiscardSummary GameCommandQueue::closeAndDiscard() {
    std::lock_guard lock(mutex_);
    accepting_ = false;
    QueueDiscardSummary summary{
        .droppedCommands = queue_.size(),
        .droppedBytes = queuedBytes_,
    };
    droppedOnStop_ += summary.droppedCommands;
    droppedBytesOnStop_ += summary.droppedBytes;
    queue_.clear();
    queuedBytes_ = 0;
    return summary;
}

}  // namespace gamenet::game_logic
