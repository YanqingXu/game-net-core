#include "gamenet/game_logic/LogicLoop.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/base/Logger.h"

#include <stdexcept>
#include <utility>

namespace gamenet::game_logic {

LogicLoop::LogicLoop(gamenet::net::EventLoop* ownerLoop, LogicLoopOptions options)
    : ownerLoop_(ownerLoop),
      ownerExecutor_(ownerLoop ? ownerLoop->executor() : gamenet::net::EventLoopExecutor{}),
      lifetimeState_(std::make_shared<LifetimeState>()),
      options_(options),
      queue_(options.queueLimits) {
    if (!ownerLoop_ || options_.tickInterval <= std::chrono::steady_clock::duration::zero() ||
        options_.maxCommandsPerTick == 0) {
        throw std::invalid_argument("LogicLoop requires owner loop and positive tick limits");
    }
}

LogicLoop::~LogicLoop() {
    lifetimeState_->revoke();
    const auto current = state_.load(std::memory_order_acquire);
    if (current == LogicLoopState::Running && !ownerExecutor_.available()) {
        LOG_FATAL << "Running LogicLoop outlived its owner EventLoop";
    }
    if (ownerExecutor_.available() && !ownerExecutor_.isInOwnerThread()) {
        LOG_FATAL << "LogicLoop destroyed outside its owner EventLoop";
    }
    if (current == LogicLoopState::Running) {
        stop();
    }
}

gamenet::net::EventLoop* LogicLoop::ownerLoop() const noexcept { return ownerLoop_; }

void LogicLoop::setHandler(Handler handler) {
    ownerLoop_->assertInLoopThread();
    handler_ = std::move(handler);
}

void LogicLoop::setOutputCallback(OutputCallback callback) {
    ownerLoop_->assertInLoopThread();
    outputCallback_ = std::move(callback);
}

void LogicLoop::setMetricCallback(MetricCallback callback) {
    ownerLoop_->assertInLoopThread();
    metricCallback_ = std::move(callback);
}

void LogicLoop::start() {
    ownerLoop_->assertInLoopThread();
    const auto current = state_.load(std::memory_order_acquire);
    if (current == LogicLoopState::Running) return;
    if (current != LogicLoopState::Created) {
        throw std::logic_error("LogicLoop is one-shot and cannot restart after stop");
    }
    if (!handler_) throw std::logic_error("LogicLoop requires a handler before start");
    const auto lifetime = lifetimeState_;
    state_.store(LogicLoopState::Running, std::memory_order_release);
    try {
        timer_ = ownerLoop_->runEvery(options_.tickInterval, [this, lifetime] {
            if (lifetime->active()) tick(lifetime);
        });
    } catch (...) {
        state_.store(LogicLoopState::Created, std::memory_order_release);
        throw;
    }
}

LogicStopSummary LogicLoop::stop() {
    ownerLoop_->assertInLoopThread();
    const auto current = state_.load(std::memory_order_acquire);
    if (current == LogicLoopState::Stopped || current == LogicLoopState::Stopping) return {};
    state_.store(LogicLoopState::Stopping, std::memory_order_release);
    if (current == LogicLoopState::Running) {
        ownerLoop_->cancel(timer_);
    }
    const auto discarded = queue_.closeAndDiscard();
    state_.store(LogicLoopState::Stopped, std::memory_order_release);
    return {
        .droppedCommands = discarded.droppedCommands,
        .droppedBytes = discarded.droppedBytes,
    };
}

SubmitResult LogicLoop::submit(GameCommand command) { return queue_.submit(std::move(command)); }
QueueSnapshot LogicLoop::queueSnapshot() const { return queue_.snapshot(); }
LogicLoopState LogicLoop::state() const noexcept { return state_.load(std::memory_order_acquire); }
bool LogicLoop::running() const noexcept { return state() == LogicLoopState::Running; }

void LogicLoop::tick(const std::shared_ptr<LifetimeState>& lifetime) {
    ownerLoop_->assertInLoopThread();
    if (!running()) return;
    // Draining commits this current batch. A callback may stop the one-shot
    // loop and discard queued backlog, but the already removed commands finish
    // in order and still produce this tick's metric snapshot.
    auto commands = queue_.drain(options_.maxCommandsPerTick);
    std::size_t produced = 0;
    for (auto& command : commands) {
        auto handler = handler_;
        auto outputCallback = outputCallback_;
        auto output = handler(std::move(command));
        if (!lifetime->active()) return;
        if (output && outputCallback) {
            ++produced;
            outputCallback(std::move(*output));
            if (!lifetime->active()) return;
        }
    }
    if (!lifetime->active()) return;
    ++tickCount_;
    auto metricCallback = metricCallback_;
    if (metricCallback) {
        const LogicTickMetric metric{
            .tick = tickCount_,
            .drained = commands.size(),
            .produced = produced,
            .queue = queue_.snapshot(),
        };
        metricCallback(metric);
    }
}

}  // namespace gamenet::game_logic
