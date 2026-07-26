#include "gamenet/core/net/EventLoop.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/Poller.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerQueue.h"
#include "gamenet/core/net/platform/Wakeup.h"
#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#endif

#include "gamenet/core/base/Logger.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
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
constexpr std::size_t kMaxControlSources = 65536;

EventLoopOptions validatedEventLoopOptions(EventLoopOptions options) {
    options.validate();
    return options;
}

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

struct EventLoopControlSource::State {
    static constexpr std::size_t kNoActiveSlot =
        (std::numeric_limits<std::size_t>::max)();

    enum class Phase {
        Accepting,
        Draining,
        Shutdown,
    };

    struct Slot {
        std::shared_ptr<EventLoop::Functor> callback;
        std::uint64_t generation{0};
        bool active{false};
    };

    State(EventLoop* loopValue, std::size_t maxSources)
        : loop(loopValue),
          slots(maxSources),
          pendingWords(maxSources / 64 + (maxSources % 64 == 0 ? 0 : 1), 0) {}

    mutable std::mutex mutex;
    EventLoop* loop;
    Phase phase{Phase::Accepting};
    std::vector<Slot> slots;
    std::vector<std::uint64_t> pendingWords;
    std::size_t pendingCount{0};
    std::size_t activeCallbackSlot{kNoActiveSlot};
    std::size_t pendingPeak{0};
    std::atomic<std::uint64_t> notifications{0};
    std::atomic<std::uint64_t> mergedNotifications{0};
    std::atomic<std::uint64_t> rejectedNotifications{0};
};

EventLoopControlSource::EventLoopControlSource(
    const std::shared_ptr<State>& state,
    std::size_t slot,
    std::uint64_t generation) noexcept
    : state_(state), slot_(slot), generation_(generation) {}

PostResult EventLoopControlSource::notify() const noexcept {
    const auto state = state_.lock();
    if (!state) {
        return PostResult::OwnerUnavailable;
    }

    std::lock_guard lock(state->mutex);
    if (slot_ >= state->slots.size()) {
        state->rejectedNotifications.fetch_add(1, std::memory_order_relaxed);
        return PostResult::OwnerUnavailable;
    }

    const auto& slot = state->slots[slot_];
    if (!slot.active || slot.generation != generation_ || state->loop == nullptr) {
        state->rejectedNotifications.fetch_add(1, std::memory_order_relaxed);
        return PostResult::OwnerUnavailable;
    }

    const bool isDrainingSelfRearm =
        state->phase == State::Phase::Draining &&
        state->loop->isInLoopThread() &&
        state->activeCallbackSlot == slot_;
    if (state->phase == State::Phase::Shutdown ||
        (state->phase == State::Phase::Draining && !isDrainingSelfRearm)) {
        state->rejectedNotifications.fetch_add(1, std::memory_order_relaxed);
        return PostResult::Shutdown;
    }

    const std::size_t wordIndex = slot_ / 64;
    const std::uint64_t mask = std::uint64_t{1} << (slot_ % 64);
    const bool alreadyPending = (state->pendingWords[wordIndex] & mask) != 0;
    state->notifications.fetch_add(1, std::memory_order_relaxed);
    if (alreadyPending) {
        state->mergedNotifications.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->pendingWords[wordIndex] |= mask;
        ++state->pendingCount;
        state->pendingPeak =
            (std::max)(state->pendingPeak, state->pendingCount);
    }

    if (!alreadyPending) {
        // The shared-state mutex linearizes this dereference against EventLoop
        // destruction. wakeup() allocates no control work and is safe
        // cross-thread. A merged notification needs no additional wakeup.
        state->loop->wakeup();
    }
    return PostResult::Accepted;
}

EventLoopExecutor::EventLoopExecutor(const std::shared_ptr<State>& state) noexcept
    : state_(state), id_(state ? state->id : 0) {}

PostResult EventLoopExecutor::postFunctor(Functor callback) const noexcept {
    if (!callback) return PostResult::QueueFull;
    const auto state = state_.lock();
    if (!state) return PostResult::OwnerUnavailable;
    try {
        std::lock_guard lock(state->mutex);
        if (state->loop == nullptr) return PostResult::OwnerUnavailable;
        if (!state->accepting) return PostResult::Shutdown;
        return state->loop->tryQueueInLoop(std::move(callback))
            ? PostResult::Accepted
            : PostResult::QueueFull;
    } catch (...) {
        return PostResult::QueueFull;
    }
}

bool EventLoopExecutor::tryQueue(Functor callback) const {
    return postFunctor(std::move(callback)) == PostResult::Accepted;
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
    if (maxControlSources > kMaxControlSources) {
        throw std::invalid_argument(
            "EventLoop control-source capacity exceeds the supported bound");
    }
}

EventLoop::EventLoop(EventLoopOptions options)
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      activeBatchEpoch_(0),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      options_(validatedEventLoopOptions(options)),
      executorState_(std::make_shared<EventLoopExecutor::State>(this)),
      controlState_(
          std::make_shared<EventLoopControlSource::State>(
              this,
              options_.maxControlSources)),
      controlDrainWords_(controlState_->pendingWords.size(), 0),
      poller_(Poller::newDefaultPoller(this)),
      timerQueue_(std::make_unique<TimerQueue>(this)),
      wakeupFds_(platform::createWakeupFds()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFds_.readFd)),
      currentActiveChannel_(nullptr),
      pendingFunctorPeak_(0),
      wakeupCount_(0),
      rejectedFunctorCount_(0),
      callbackExceptionCount_(0) {
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
        std::unique_lock executorLock(executorState_->mutex);
        std::unique_lock controlLock(controlState_->mutex);
        executorState_->accepting = false;
        executorState_->drainingAccepted = false;
        executorState_->loop = nullptr;
        controlState_->phase = EventLoopControlSource::State::Phase::Shutdown;
        controlState_->loop = nullptr;
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
        const int pollTimeout =
            hasPendingControlSources() ? 0 : timerQueue_->pollTimeoutMs(10000);
        pollReturnTime_ = poller_->poll(pollTimeout, &activeChannels_);
        dispatchActiveChannels();
        for (auto& exception : timerQueue_->handleExpired(gamenet::base::now())) {
            handleCallbackException(EventLoopCallbackSource::Timer, exception);
        }
        doControlSources();
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
        const bool hasControl = hasPendingControlSources();
        if (!hasPending && !hasControl) {
            break;
        }
        if (hasControl) {
            doControlSources();
        }
        if (hasPending) {
            doPendingFunctors(options_.maxFunctorsPerIteration);
        }
    }

    {
        // Publish final-drain completion and control shutdown as one state
        // transition. A concurrent/repeated quit must observe both together
        // and cannot resurrect executor owner-drain identity.
        std::unique_lock executorLock(executorState_->mutex);
        std::unique_lock controlLock(controlState_->mutex);
        executorState_->drainingAccepted = false;
        controlState_->phase = EventLoopControlSource::State::Phase::Shutdown;
    }

    looping_ = false;
}

void EventLoop::quit() {
    const bool crossThread = !isInLoopThread();
    // Keep the executor->control lock order shared with final shutdown and
    // destruction. This makes the two admission planes and final-drain owner
    // identity one observable transition.
    std::unique_lock executorLock(executorState_->mutex);
    std::unique_lock controlLock(controlState_->mutex);
    executorState_->accepting = false;
    if (controlState_->phase == EventLoopControlSource::State::Phase::Shutdown) {
        executorState_->drainingAccepted = false;
        quit_.store(true, std::memory_order_relaxed);
        return;
    }
    executorState_->drainingAccepted = true;
    controlState_->phase = EventLoopControlSource::State::Phase::Draining;
    quit_.store(true, std::memory_order_relaxed);
    if (crossThread) {
        // Keep the shared-state lock across wakeup so EventLoop destruction
        // cannot invalidate Poller/wakeup storage until this call returns.
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

EventLoopControlSource EventLoop::registerControlSource(Functor cb) {
    assertInLoopThread();
    if (looping_) {
        throw std::logic_error(
            "EventLoop control sources must be registered before loop() starts");
    }
    if (!cb) {
        throw std::invalid_argument(
            "EventLoop control source requires a non-empty callback");
    }

    auto callback = std::make_shared<Functor>(std::move(cb));
    std::lock_guard lock(controlState_->mutex);
    if (controlState_->phase !=
        EventLoopControlSource::State::Phase::Accepting) {
        throw std::logic_error(
            "EventLoop control-source registration is closed");
    }

    for (std::size_t slotIndex = 0;
         slotIndex < controlState_->slots.size();
         ++slotIndex) {
        auto& slot = controlState_->slots[slotIndex];
        if (slot.active) {
            continue;
        }

        ++slot.generation;
        if (slot.generation == 0) {
            ++slot.generation;
        }
        slot.callback = std::move(callback);
        slot.active = true;
        return EventLoopControlSource(
            controlState_,
            slotIndex,
            slot.generation);
    }

    throw std::length_error(
        "EventLoop control-source registration capacity is exhausted");
}

void EventLoop::unregisterControlSource(
    const EventLoopControlSource& source) {
    assertInLoopThread();
    const auto state = source.state_.lock();
    if (!state || state.get() != controlState_.get()) {
        return;
    }

    std::shared_ptr<Functor> callback;
    {
        std::lock_guard lock(state->mutex);
        if (source.slot_ >= state->slots.size()) {
            return;
        }

        auto& slot = state->slots[source.slot_];
        if (!slot.active || slot.generation != source.generation_) {
            return;
        }

        const std::size_t wordIndex = source.slot_ / 64;
        const std::uint64_t mask =
            std::uint64_t{1} << (source.slot_ % 64);
        if ((state->pendingWords[wordIndex] & mask) != 0) {
            state->pendingWords[wordIndex] &= ~mask;
            --state->pendingCount;
        }

        callback = std::move(slot.callback);
        slot.active = false;
        ++slot.generation;
        if (slot.generation == 0) {
            ++slot.generation;
        }
    }
}

std::size_t EventLoop::pendingControlSourceCount() const {
    std::lock_guard lock(controlState_->mutex);
    return controlState_->pendingCount;
}

std::uint64_t EventLoop::controlNotificationCount() const noexcept {
    return controlState_->notifications.load(std::memory_order_relaxed);
}

std::uint64_t EventLoop::mergedControlNotificationCount() const noexcept {
    return controlState_->mergedNotifications.load(std::memory_order_relaxed);
}

std::uint64_t EventLoop::rejectedControlNotificationCount() const noexcept {
    return controlState_->rejectedNotifications.load(
        std::memory_order_relaxed);
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
    if (channel == nullptr || channel->ownerLoop() != this) {
        throw std::invalid_argument(
            "EventLoop::updateChannel requires a Channel owned by this loop");
    }
    const bool newRegistration = !channel->addedToLoop_;
    poller_->updateChannel(channel);
    if (newRegistration) {
        channel->addedToLoop_ = true;
        channel->advanceRegistrationGeneration();
    }
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    if (channel == nullptr || channel->ownerLoop() != this) {
        throw std::invalid_argument(
            "EventLoop::removeChannel requires a Channel owned by this loop");
    }
    if (!channel->addedToLoop_) {
        throw std::logic_error(
            "EventLoop::removeChannel requires an active registration");
    }
    if (!channel->isNoneEvent()) {
        throw std::logic_error(
            "EventLoop::removeChannel requires disabled interests");
    }
    if (!poller_->hasChannel(channel)) {
        throw std::logic_error(
            "EventLoop::removeChannel registration identity mismatch");
    }

    poller_->removeChannel(channel);

    if (eventHandling_ && channel->activeBatchEpoch_ == activeBatchEpoch_) {
        const std::size_t index = channel->activeBatchIndex_;
        if (index >= activeChannels_.size() ||
            activeChannels_[index] != channel) {
            LOG_FATAL << "EventLoop active Channel batch membership is corrupt";
        }
        activeChannels_[index] = nullptr;
    }
    channel->addedToLoop_ = false;
    channel->advanceRegistrationGeneration();
}

void EventLoop::preserveSocketAssociation(SocketFd sockfd) {
    assertInLoopThread();
    poller_->preserveSocketAssociation(sockfd);
}

void EventLoop::forgetSocketAssociation(SocketFd sockfd) noexcept {
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop socket association rollback used from a different thread";
    }
#ifdef _WIN32
    if (auto* iocp = dynamic_cast<IocpPoller*>(poller_.get())) {
        iocp->forgetSocketAssociation(sockfd);
    }
#else
    (void)sockfd;
#endif
}

void EventLoop::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    assertInLoopThread();
    poller_->retainCompletionOperation(operation, std::move(lifetime));
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

void EventLoop::dispatchActiveChannels() {
    assertInLoopThread();
    if (eventHandling_) {
        throw std::logic_error("EventLoop active Channel dispatch cannot re-enter");
    }

    ++activeBatchEpoch_;
    if (activeBatchEpoch_ == 0) {
        ++activeBatchEpoch_;
    }

    for (std::size_t index = 0; index < activeChannels_.size(); ++index) {
        Channel* channel = activeChannels_[index];
        if (channel == nullptr) {
            continue;
        }
        if (channel->activeBatchEpoch_ == activeBatchEpoch_) {
            LOG_FATAL << "Poller returned one Channel more than once in an active batch";
        }
        channel->activeBatchEpoch_ = activeBatchEpoch_;
        channel->activeBatchIndex_ = index;
    }

    eventHandling_ = true;
    for (Channel* channel : activeChannels_) {
        if (channel == nullptr) {
            continue;
        }
        currentActiveChannel_ = channel;
        try {
            channel->handleEvent(pollReturnTime_);
        } catch (...) {
            handleCallbackException(
                EventLoopCallbackSource::ChannelEvent,
                std::current_exception());
        }
        currentActiveChannel_ = nullptr;
        retiredCurrentChannel_.reset();
    }
    eventHandling_ = false;
}

void EventLoop::retireCurrentChannel(
    std::unique_ptr<Channel> channel) noexcept {
    if (!channel) {
        return;
    }
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop current Channel retirement used from a different thread";
    }
    if (!eventHandling_ || currentActiveChannel_ != channel.get()) {
        return;
    }
    if (retiredCurrentChannel_) {
        LOG_FATAL << "EventLoop current Channel retirement slot is already occupied";
    }
    retiredCurrentChannel_ = std::move(channel);
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

bool EventLoop::hasPendingControlSources() const {
    std::lock_guard lock(controlState_->mutex);
    return controlState_->pendingCount != 0;
}

void EventLoop::doControlSources() {
    assertInLoopThread();

    std::size_t pendingCount = 0;
    std::size_t pendingPeak = 0;
    {
        std::lock_guard lock(controlState_->mutex);
        std::copy(
            controlState_->pendingWords.begin(),
            controlState_->pendingWords.end(),
            controlDrainWords_.begin());
        std::fill(
            controlState_->pendingWords.begin(),
            controlState_->pendingWords.end(),
            std::uint64_t{0});
        pendingCount = controlState_->pendingCount;
        controlState_->pendingCount = 0;
        pendingPeak = controlState_->pendingPeak;
        controlState_->pendingPeak = 0;
    }

    if (pendingCount == 0) {
        return;
    }

    for (std::size_t wordIndex = 0;
         wordIndex < controlDrainWords_.size();
         ++wordIndex) {
        auto word = controlDrainWords_[wordIndex];
        while (word != 0) {
            const auto bitIndex =
                static_cast<std::size_t>(std::countr_zero(word));
            const std::size_t slotIndex = wordIndex * 64 + bitIndex;
            word &= word - 1;

            std::shared_ptr<Functor> callback;
            {
                std::lock_guard lock(controlState_->mutex);
                if (slotIndex >= controlState_->slots.size()) {
                    continue;
                }
                const auto& slot = controlState_->slots[slotIndex];
                if (!slot.active || !slot.callback) {
                    continue;
                }
                controlState_->activeCallbackSlot = slotIndex;
                callback = slot.callback;
            }

            std::exception_ptr exception;
            try {
                (*callback)();
            } catch (...) {
                exception = std::current_exception();
            }

            {
                std::lock_guard lock(controlState_->mutex);
                if (controlState_->activeCallbackSlot == slotIndex) {
                    controlState_->activeCallbackSlot =
                        EventLoopControlSource::State::kNoActiveSlot;
                }
            }

            if (exception) {
                handleCallbackException(
                    EventLoopCallbackSource::Control,
                    exception);
            }
        }
        controlDrainWords_[wordIndex] = 0;
    }

    EventLoopMetricSample sample;
    sample.event = EventLoopMetricEvent::ControlSourcesDrained;
    sample.pendingControlSources = pendingCount;
    sample.pendingControlSourcePeak =
        (std::max)(pendingPeak, pendingCount);
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
    sample.controlNotifications =
        controlState_->notifications.load(std::memory_order_relaxed);
    sample.mergedControlNotifications =
        controlState_->mergedNotifications.load(std::memory_order_relaxed);
    sample.rejectedControlNotifications =
        controlState_->rejectedNotifications.load(std::memory_order_relaxed);
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
            quit();
        }
    } catch (const std::exception& error) {
        LOG_ERROR << "EventLoop callback exception handler threw: " << error.what();
        quit();
    } catch (...) {
        LOG_ERROR << "EventLoop callback exception handler threw a non-standard exception";
        quit();
    }
}

}  // namespace gamenet::net
