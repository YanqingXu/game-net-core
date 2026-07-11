#pragma once

#include "gamenet/core/net/TimerId.h"
#include "gamenet/game_logic/GameCommandQueue.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::game_logic {

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

    LogicLoop(gamenet::net::EventLoop* ownerLoop, LogicLoopOptions options = {});
    ~LogicLoop();

    LogicLoop(const LogicLoop&) = delete;
    LogicLoop& operator=(const LogicLoop&) = delete;

    gamenet::net::EventLoop* ownerLoop() const noexcept;
    void setHandler(Handler handler);
    void setOutputCallback(OutputCallback callback);
    void setMetricCallback(MetricCallback callback);
    void start();
    void stop();

    SubmitResult submit(GameCommand command);
    QueueSnapshot queueSnapshot() const;
    bool running() const noexcept;

private:
    void tick();

    gamenet::net::EventLoop* ownerLoop_;
    LogicLoopOptions options_;
    GameCommandQueue queue_;
    Handler handler_;
    OutputCallback outputCallback_;
    MetricCallback metricCallback_;
    gamenet::net::TimerId timer_;
    std::size_t tickCount_{};
    bool running_{false};
};

}  // namespace gamenet::game_logic
