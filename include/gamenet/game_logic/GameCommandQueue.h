#pragma once

#include "gamenet/game_logic/GameCommand.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace gamenet::game_logic {

enum class SubmitResult {
    Accepted,
    QueueFull,
    PayloadTooLarge,
    Stopped,
};

struct QueueLimits {
    std::size_t maxCommands{4096};
    std::size_t maxQueuedBytes{4U * 1024U * 1024U};
    std::size_t maxPayloadBytes{1024U * 1024U};
};

struct QueueSnapshot {
    std::size_t depth{};
    std::size_t queuedBytes{};
    std::size_t depthHighWatermark{};
    std::size_t bytesHighWatermark{};
    std::size_t accepted{};
    std::size_t rejectedFull{};
    std::size_t rejectedPayload{};
    std::size_t rejectedStopped{};
    std::size_t droppedOnStop{};
    std::size_t droppedBytesOnStop{};
    bool accepting{};
};

struct QueueDiscardSummary {
    std::size_t droppedCommands{};
    std::size_t droppedBytes{};
};

class GameCommandQueue {
public:
    explicit GameCommandQueue(QueueLimits limits = {});

    SubmitResult submit(GameCommand command);
    std::vector<GameCommand> drain(std::size_t maxCommands);
    QueueSnapshot snapshot() const;
    void close();
    QueueDiscardSummary closeAndDiscard();

private:
    QueueLimits limits_;
    mutable std::mutex mutex_;
    std::deque<GameCommand> queue_;
    std::size_t queuedBytes_{};
    std::size_t depthHighWatermark_{};
    std::size_t bytesHighWatermark_{};
    std::size_t accepted_{};
    std::size_t rejectedFull_{};
    std::size_t rejectedPayload_{};
    std::size_t rejectedStopped_{};
    std::size_t droppedOnStop_{};
    std::size_t droppedBytesOnStop_{};
    bool accepting_{true};
};

}  // namespace gamenet::game_logic
