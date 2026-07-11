#include "gamenet/game_logic/LogicLoop.h"

#include "gamenet/core/net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace gamenet::game_logic {

LogicLoop::LogicLoop(gamenet::net::EventLoop* ownerLoop, LogicLoopOptions options)
    : ownerLoop_(ownerLoop), options_(options), queue_(options.queueLimits) {
    if (!ownerLoop_ || options_.tickInterval <= std::chrono::steady_clock::duration::zero() ||
        options_.maxCommandsPerTick == 0) {
        throw std::invalid_argument("LogicLoop requires owner loop and positive tick limits");
    }
}

LogicLoop::~LogicLoop() {
    if (running_) {
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
    if (running_) return;
    if (!handler_) throw std::logic_error("LogicLoop requires a handler before start");
    running_ = true;
    timer_ = ownerLoop_->runEvery(options_.tickInterval, [this] { tick(); });
}

void LogicLoop::stop() {
    ownerLoop_->assertInLoopThread();
    if (!running_) return;
    running_ = false;
    queue_.close();
    ownerLoop_->cancel(timer_);
}

SubmitResult LogicLoop::submit(GameCommand command) { return queue_.submit(std::move(command)); }
QueueSnapshot LogicLoop::queueSnapshot() const { return queue_.snapshot(); }
bool LogicLoop::running() const noexcept { return running_; }

void LogicLoop::tick() {
    ownerLoop_->assertInLoopThread();
    if (!running_) return;
    auto commands = queue_.drain(options_.maxCommandsPerTick);
    std::size_t produced = 0;
    for (auto& command : commands) {
        auto output = handler_(std::move(command));
        if (output && outputCallback_) {
            ++produced;
            outputCallback_(std::move(*output));
        }
    }
    ++tickCount_;
    if (metricCallback_) {
        metricCallback_({
            .tick = tickCount_,
            .drained = commands.size(),
            .produced = produced,
            .queue = queue_.snapshot(),
        });
    }
}

}  // namespace gamenet::game_logic
