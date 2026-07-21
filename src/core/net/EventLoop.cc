#include "gamenet/core/net/EventLoop.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/Poller.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerQueue.h"
#include "gamenet/core/net/platform/Wakeup.h"

#include "gamenet/core/base/Logger.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace gamenet::net {

namespace {

thread_local EventLoop* t_loopInThisThread = nullptr;
std::atomic<std::uint64_t> nextExecutorId{1};

}  // namespace

struct EventLoopExecutor::State {
    explicit State(EventLoop* loopValue)
        : loop(loopValue), id(nextExecutorId.fetch_add(1, std::memory_order_relaxed)) {}

    mutable std::mutex mutex;
    EventLoop* loop;
    std::uint64_t id;
    bool accepting{true};
    bool drainingAccepted{false};
};

EventLoopExecutor::EventLoopExecutor(const std::shared_ptr<State>& state) noexcept
    : state_(state), id_(state ? state->id : 0) {}

bool EventLoopExecutor::tryQueue(Functor callback) const {
    if (!callback) return false;
    const auto state = state_.lock();
    if (!state) return false;
    std::lock_guard lock(state->mutex);
    if (!state->accepting || state->loop == nullptr) return false;
    return state->loop->tryQueueInLoop(std::move(callback));
}

bool EventLoopExecutor::available() const noexcept {
    const auto state = state_.lock();
    if (!state) return false;
    std::lock_guard lock(state->mutex);
    return state->accepting && state->loop != nullptr;
}

bool EventLoopExecutor::isInOwnerThread() const noexcept {
    const auto state = state_.lock();
    if (!state) return false;
    std::lock_guard lock(state->mutex);
    return (state->accepting || state->drainingAccepted) && state->loop != nullptr &&
           state->loop->isInLoopThread();
}

std::uint64_t EventLoopExecutor::id() const noexcept { return id_; }

void EventLoopOptions::validate() const {
    if (maxPendingFunctors == 0) {
        throw std::invalid_argument("EventLoop max pending functors must be positive");
    }
    if (reservedPendingFunctors >
        (std::numeric_limits<std::size_t>::max)() - maxPendingFunctors) {
        throw std::invalid_argument("EventLoop pending functor capacity overflows size_t");
    }
    if (maxFunctorsPerIteration == 0 ||
        maxFunctorsPerIteration > maxPendingFunctors) {
        throw std::invalid_argument(
            "EventLoop per-iteration functor budget must be within queue capacity");
    }
}

EventLoop::EventLoop(EventLoopOptions options)
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      executorState_(std::make_shared<EventLoopExecutor::State>(this)),
      poller_(Poller::newDefaultPoller(this)),
      timerQueue_(std::make_unique<TimerQueue>(this)),
      wakeupFds_(platform::createWakeupFds()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFds_.readFd)),
      currentActiveChannel_(nullptr),
      options_(options),
      pendingFunctorPeak_(0),
      wakeupCount_(0),
      rejectedFunctorCount_(0),
      callbackExceptionCount_(0) {
    options_.validate();
    if (t_loopInThisThread != nullptr) {
        throw std::runtime_error("another EventLoop already exists in this thread");
    }
    t_loopInThisThread = this;

    wakeupChannel_->setReadCallback([this](gamenet::base::Timestamp receiveTime) { handleRead(receiveTime); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop destroyed from non-owner thread";
    }
    if (looping_) {
        LOG_FATAL << "EventLoop destroyed while loop() is still running";
    }
    {
        std::lock_guard lock(executorState_->mutex);
        executorState_->accepting = false;
        executorState_->loop = nullptr;
    }
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    platform::closeWakeupFds(wakeupFds_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;

    while (!quit_) {
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(timerQueue_->pollTimeoutMs(10000), &activeChannels_);
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            try {
                channel->handleEvent(pollReturnTime_);
            } catch (...) {
                handleCallbackException(
                    EventLoopCallbackSource::ChannelEvent,
                    std::current_exception());
            }
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;
        for (auto& exception : timerQueue_->handleExpired(gamenet::base::now())) {
            handleCallbackException(EventLoopCallbackSource::Timer, exception);
        }
        doPendingFunctors(options_.maxFunctorsPerIteration);
    }

    {
        std::lock_guard lock(executorState_->mutex);
        executorState_->accepting = false;
        executorState_->drainingAccepted = true;
    }

    while (true) {
        bool hasPending = false;
        {
            std::lock_guard lock(mutex_);
            hasPending = !pendingFunctors_.empty();
        }
        if (!hasPending) {
            break;
        }
        doPendingFunctors(options_.maxFunctorsPerIteration);
    }

    {
        std::lock_guard lock(executorState_->mutex);
        executorState_->drainingAccepted = false;
    }

    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) {
        wakeup();
    }
}

gamenet::base::Timestamp EventLoop::pollReturnTime() const noexcept {
    return pollReturnTime_;
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    if (!tryQueueInLoopImpl(std::move(cb), true)) {
        throw std::overflow_error("EventLoop pending functor queue is full");
    }
}

bool EventLoop::tryQueueInLoop(Functor cb) {
    return tryQueueInLoopImpl(std::move(cb), false);
}

bool EventLoop::tryQueueInLoopImpl(Functor cb, bool allowReserve) {
    if (!cb) {
        return false;
    }
    const auto enqueuedAt = gamenet::base::now();
    {
        std::lock_guard lock(mutex_);
        const std::size_t capacity = options_.maxPendingFunctors +
            (allowReserve ? options_.reservedPendingFunctors : 0);
        if (pendingFunctors_.size() >= capacity) {
            rejectedFunctorCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        pendingFunctors_.push_back(PendingFunctor{std::move(cb), enqueuedAt});
        const auto pendingSize = pendingFunctors_.size();
        auto observedPeak = pendingFunctorPeak_.load(std::memory_order_relaxed);
        while (pendingSize > observedPeak &&
               !pendingFunctorPeak_.compare_exchange_weak(
                   observedPeak,
                   pendingSize,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    if (!isInLoopThread() || callingPendingFunctors_.load(std::memory_order_relaxed)) {
        wakeup();
    }
    return true;
}

std::size_t EventLoop::pendingFunctorCount() const {
    std::lock_guard lock(mutex_);
    return pendingFunctors_.size();
}

std::uint64_t EventLoop::rejectedFunctorCount() const noexcept {
    return rejectedFunctorCount_.load(std::memory_order_relaxed);
}

EventLoopExecutor EventLoop::executor() const noexcept {
    return EventLoopExecutor(executorState_);
}

void EventLoop::setEventLoopMetricCallback(EventLoopMetricCallback cb) {
    assertInLoopThread();
    eventLoopMetricCallback_ = std::move(cb);
}

void EventLoop::setCallbackExceptionHandler(EventLoopCallbackExceptionHandler cb) {
    assertInLoopThread();
    callbackExceptionHandler_ = std::move(cb);
}

std::uint64_t EventLoop::callbackExceptionCount() const noexcept {
    return callbackExceptionCount_.load(std::memory_order_relaxed);
}

TimerId EventLoop::runAt(gamenet::base::Timestamp time, Functor cb) {
    return timerQueue_->addTimer(std::move(cb), time);
}

TimerId EventLoop::runAfter(TimerDuration delay, Functor cb) {
    return timerQueue_->addTimer(std::move(cb), gamenet::base::now() + delay, TimerDuration::zero());
}

TimerId EventLoop::runEvery(TimerDuration interval, Functor cb) {
    if (interval <= TimerDuration::zero()) {
        throw std::invalid_argument("runEvery interval must be positive");
    }
    return timerQueue_->addTimer(std::move(cb), gamenet::base::now() + interval, interval);
}

void EventLoop::cancel(TimerId timerId) {
    timerQueue_->cancel(timerId);
}

void EventLoop::wakeup() {
    wakeupCount_.fetch_add(1, std::memory_order_relaxed);
    if (poller_->wakeup()) {
        return;
    }

    const ssize_t written = platform::writeWakeup(wakeupFds_.writeFd);
    if (written < 0 && !sockets::isWouldBlock(sockets::lastError())) {
        LOG_SYSERR << "EventLoop::wakeup: " << sockets::errorMessage(sockets::lastError());
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

void EventLoop::preserveSocketAssociation(SocketFd sockfd) {
    assertInLoopThread();
    poller_->preserveSocketAssociation(sockfd);
}

void EventLoop::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    assertInLoopThread();
    poller_->retainCompletionOperation(operation, std::move(lifetime));
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

bool EventLoop::isInLoopThread() const noexcept {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        throw std::runtime_error("EventLoop used from a different thread");
    }
}

void EventLoop::handleRead(gamenet::base::Timestamp receiveTime) {
    (void)receiveTime;
    if (!platform::drainWakeup(wakeupFds_.readFd) && !sockets::isWouldBlock(sockets::lastError())) {
        LOG_SYSERR << "EventLoop::handleRead: " << sockets::errorMessage(sockets::lastError());
    }
    EventLoopMetricSample sample;
    sample.event = EventLoopMetricEvent::WakeupHandled;
    sample.loop = this;
    sample.wakeupCount = wakeupCount_.load(std::memory_order_relaxed);
    emitEventLoopMetric(sample);
}

void EventLoop::doPendingFunctors(std::size_t maxCount) {
    std::vector<PendingFunctor> functors;
    std::size_t pendingPeak = 0;
    callingPendingFunctors_.store(true, std::memory_order_relaxed);

    {
        std::lock_guard lock(mutex_);
        const std::size_t count = std::min(maxCount, pendingFunctors_.size());
        functors.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            functors.push_back(std::move(pendingFunctors_.front()));
            pendingFunctors_.pop_front();
        }
        pendingPeak = pendingFunctorPeak_.exchange(0, std::memory_order_relaxed);
    }

    if (!functors.empty()) {
        const auto now = gamenet::base::now();
        EventLoopMetricSample sample;
        sample.event = EventLoopMetricEvent::PendingFunctorsDrained;
        sample.loop = this;
        sample.pendingFunctors = functors.size();
        sample.pendingFunctorPeak = std::max(pendingPeak, functors.size());
        sample.wakeupCount = wakeupCount_.load(std::memory_order_relaxed);
        sample.rejectedFunctors = rejectedFunctorCount_.load(std::memory_order_relaxed);
        sample.callbackExceptions = callbackExceptionCount_.load(std::memory_order_relaxed);
        sample.oldestPendingLatency = now - functors.front().enqueuedAt;
        emitEventLoopMetric(sample);
    }

    for (auto& functor : functors) {
        try {
            functor.functor();
        } catch (...) {
            handleCallbackException(
                EventLoopCallbackSource::PendingFunctor,
                std::current_exception());
        }
    }

    callingPendingFunctors_.store(false, std::memory_order_relaxed);

    if (pendingFunctorCount() > 0) {
        wakeup();
    }
}

void EventLoop::emitEventLoopMetric(EventLoopMetricSample sample) {
    if (!eventLoopMetricCallback_) {
        return;
    }
    sample.loop = this;
    sample.callbackExceptions = callbackExceptionCount_.load(std::memory_order_relaxed);
    try {
        eventLoopMetricCallback_(sample);
    } catch (...) {
        handleCallbackException(
            EventLoopCallbackSource::Metric,
            std::current_exception());
    }
}

void EventLoop::handleCallbackException(
    EventLoopCallbackSource source,
    std::exception_ptr exception) noexcept {
    callbackExceptionCount_.fetch_add(1, std::memory_order_relaxed);
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
        LOG_ERROR << "EventLoop callback threw an empty exception";
    } catch (const std::exception& error) {
        LOG_ERROR << "EventLoop callback exception: " << error.what();
    } catch (...) {
        LOG_ERROR << "EventLoop callback threw a non-standard exception";
    }

    if (!callbackExceptionHandler_) {
        return;
    }

    try {
        if (callbackExceptionHandler_(EventLoopCallbackException{
                .source = source,
                .exception = exception,
            }) == EventLoopCallbackExceptionAction::Quit) {
            quit_ = true;
        }
    } catch (const std::exception& error) {
        LOG_ERROR << "EventLoop callback exception handler threw: " << error.what();
        quit_ = true;
    } catch (...) {
        LOG_ERROR << "EventLoop callback exception handler threw a non-standard exception";
        quit_ = true;
    }
}

}  // namespace gamenet::net
