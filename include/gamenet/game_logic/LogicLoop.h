#pragma once

#include "gamenet/core/net/TimerId.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/game_logic/GameCommandQueue.h"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::game_logic {

enum class LogicLoopState {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct LogicStopSummary {
    std::size_t droppedCommands{};
    std::size_t droppedBytes{};
};

struct LogicLoopOptions {
    std::chrono::steady_clock::duration tickInterval{std::chrono::milliseconds(20)};
    std::size_t maxCommandsPerTick{256};
    QueueLimits queueLimits{};
};

struct LogicTickMetric {
    std::size_t tick{};
    std::size_t drained{};
    std::size_t produced{};
    QueueSnapshot queue;
};

class LogicLoop {
public:
    using Handler = std::function<std::optional<GameCommand>(GameCommand)>;
    using OutputCallback = std::function<void(GameCommand)>;
    using MetricCallback = std::function<void(const LogicTickMetric&)>;

    // The caller owns ownerLoop. It must remain alive and accepting until a
    // Running LogicLoop is stopped or destroyed on that owner thread.
    LogicLoop(gamenet::net::EventLoop* ownerLoop, LogicLoopOptions options = {});
    ~LogicLoop();

    LogicLoop(const LogicLoop&) = delete;
    LogicLoop& operator=(const LogicLoop&) = delete;

    gamenet::net::EventLoop* ownerLoop() const noexcept;
    void setHandler(Handler handler);
    void setOutputCallback(OutputCallback callback);
    void setMetricCallback(MetricCallback callback);
    void start();
    LogicStopSummary stop();

    SubmitResult submit(GameCommand command);
    QueueSnapshot queueSnapshot() const;
    LogicLoopState state() const noexcept;
    bool running() const noexcept;

private:
    struct LifetimeState {
        bool active() const noexcept { return alive.load(std::memory_order_acquire); }
        void revoke() noexcept { alive.store(false, std::memory_order_release); }

        std::atomic<bool> alive{true};
    };

    void tick(const std::shared_ptr<LifetimeState>& lifetime);

    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::EventLoopExecutor ownerExecutor_;
    std::shared_ptr<LifetimeState> lifetimeState_;
    LogicLoopOptions options_;
    GameCommandQueue queue_;
    Handler handler_;
    OutputCallback outputCallback_;
    MetricCallback metricCallback_;
    gamenet::net::TimerId timer_;
    std::size_t tickCount_{};
    std::atomic<LogicLoopState> state_{LogicLoopState::Created};
};

}  // namespace gamenet::game_logic
