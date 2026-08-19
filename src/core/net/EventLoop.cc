#include "gamenet/core/net/EventLoop.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/Poller.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerQueue.h"
#include "gamenet/core/net/platform/Wakeup.h"
#include "detail/IoEngine.h"

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
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamenet::net {

namespace {

thread_local EventLoop* t_loopInThisThread = nullptr;
std::atomic<std::uint64_t> nextExecutorId{1};
constexpr std::size_t kMaxControlSources = 65536;
constexpr std::size_t kMaxIocpCompletionBatchSize = 64;

EventLoopOptions validatedEventLoopOptions(EventLoopOptions options) {
    options.validate();
    return options;
}

bool ioEngineAcceptedOrUnsupported(
    detail::IoEngineOperationResult result) noexcept {
    return detail::accepted(result) ||
        result == detail::IoEngineOperationResult::RejectedUnsupported;
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
        gamenet::base::Timestamp pendingSince{};
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

struct EventLoopLifecycleSource::Node {
    std::shared_ptr<EventLoop::Functor> callback;
    gamenet::base::Timestamp pendingSince{};
    Node* dirtyNext{nullptr};
    std::uint64_t id{0};
    std::uint64_t generation{1};
    std::uint64_t pendingGeneration{0};
    bool attached{true};
    bool dirty{false};
    bool active{false};
};

struct EventLoopLifecycleSource::State {
    State(EventLoop* loopValue, std::size_t maxNodesValue)
        : loop(loopValue), maxNodes(maxNodesValue) {}

    mutable std::mutex mutex;
    EventLoop* loop;
    EventLoopPhase phase{EventLoopPhase::Running};
    const std::size_t maxNodes;
    std::uint64_t nextNodeId{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<Node>> nodes;
    Node* dirtyHead{nullptr};
    Node* dirtyTail{nullptr};
    Node* activeNode{nullptr};
    std::uint64_t activeGeneration{0};
    std::size_t attachedCount{0};
    std::size_t pendingCount{0};
    std::atomic<std::uint64_t> signals{0};
    std::atomic<std::uint64_t> mergedSignals{0};
    std::atomic<std::uint64_t> rejectedSignals{0};
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
        state->slots[slot_].pendingSince = gamenet::base::now();
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

EventLoopLifecycleSource::EventLoopLifecycleSource(
    const std::shared_ptr<State>& state,
    const std::shared_ptr<Node>& node,
    std::uint64_t generation) noexcept
    : state_(state), node_(node), generation_(generation) {}

PostResult EventLoopLifecycleSource::signal() const noexcept {
    const auto state = state_.lock();
    const auto node = node_.lock();
    if (!state || !node) {
        return PostResult::OwnerUnavailable;
    }

    std::lock_guard lock(state->mutex);
    if (state->loop == nullptr ||
        !node->attached ||
        node->generation != generation_) {
        state->rejectedSignals.fetch_add(1, std::memory_order_relaxed);
        return PostResult::OwnerUnavailable;
    }

    const bool isDrainingSelfSignal =
        (state->phase == EventLoopPhase::Quiescing ||
         state->phase == EventLoopPhase::FinalDraining) &&
        state->loop->isInLoopThread() &&
        state->activeNode == node.get() &&
        state->activeGeneration == generation_;
    if (state->phase == EventLoopPhase::Shutdown ||
        (state->phase != EventLoopPhase::Running &&
         !isDrainingSelfSignal)) {
        state->rejectedSignals.fetch_add(1, std::memory_order_relaxed);
        return PostResult::Shutdown;
    }

    state->signals.fetch_add(1, std::memory_order_relaxed);
    if (node->dirty) {
        state->mergedSignals.fetch_add(1, std::memory_order_relaxed);
        return PostResult::Accepted;
    }

    node->dirty = true;
    node->pendingSince = gamenet::base::now();
    node->pendingGeneration = generation_;
    node->dirtyNext = nullptr;
    if (state->dirtyTail == nullptr) {
        state->dirtyHead = node.get();
    } else {
        state->dirtyTail->dirtyNext = node.get();
    }
    state->dirtyTail = node.get();
    ++state->pendingCount;

    // The lifecycle-state lock linearizes this dereference with EventLoop
    // destruction. The embedded dirty link means signal allocates no queue
    // node, and one wakeup is sufficient for the newly committed generation.
    state->loop->wakeup();
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
        return state->loop->tryQueueInLoopImpl(std::move(callback), false)
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
    if (maxLifecycleCallbacksPerIteration == 0) {
        throw std::invalid_argument(
            "EventLoop lifecycle callback budget must be positive");
    }
    if (maxActiveChannelsPerIteration == 0) {
        throw std::invalid_argument(
            "EventLoop active Channel budget must be positive");
    }
    if (maxTimersPerIteration == 0) {
        throw std::invalid_argument(
            "EventLoop expired timer budget must be positive");
    }
    if (maxControlCallbacksPerIteration == 0 ||
        maxControlCallbacksPerIteration > kMaxControlSources) {
        throw std::invalid_argument(
            "EventLoop control callback budget must be within the supported bound");
    }
    if (maxIocpCompletionsPerPoll == 0 ||
        maxIocpCompletionsPerPoll > kMaxIocpCompletionBatchSize) {
        throw std::invalid_argument(
            "EventLoop IOCP completion budget must be within [1, 64]");
    }
}

EventLoop::EventLoop(EventLoopOptions options)
    : looping_(false),
      quit_(false),
      phase_(EventLoopPhase::Running),
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
      lifecycleState_(
          std::make_shared<EventLoopLifecycleSource::State>(
              this,
              options_.maxLifecycleNodes)),
      controlDrainWords_(controlState_->pendingWords.size(), 0),
      poller_(detail::makePollerIoEngineAdapter(
          this,
          detail::IoEngineOptions{
              .maxCompletionNoticesPerWait =
                  options_.maxIocpCompletionsPerPoll,
          })),
      timerQueue_(std::unique_ptr<TimerQueue>(new TimerQueue(this))),
#ifdef _WIN32
      wakeupFds_(platform::createWakeupFds()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFds_.readFd)),
#else
      wakeupFds_(),
      wakeupChannel_(),
#endif
      activeChannelCursor_(0),
      currentActiveChannel_(nullptr),
      pendingFunctorPeak_(0),
      wakeupCount_(0),
      rejectedFunctorCount_(0),
      callbackExceptionCount_(0) {
    if (t_loopInThisThread != nullptr) {
        throw std::runtime_error("another EventLoop already exists in this thread");
    }
    t_loopInThisThread = this;

    if (wakeupChannel_) {
        wakeupChannel_->setReadCallback(
            [this](gamenet::base::Timestamp receiveTime) {
                handleRead(receiveTime);
            });
        wakeupChannel_->enableReading();
    }
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
        std::unique_lock lifecycleLock(lifecycleState_->mutex);
        executorState_->accepting = false;
        executorState_->drainingAccepted = false;
        executorState_->loop = nullptr;
        controlState_->phase = EventLoopControlSource::State::Phase::Shutdown;
        controlState_->loop = nullptr;
        lifecycleState_->phase = EventLoopPhase::Shutdown;
        lifecycleState_->loop = nullptr;
        phase_.store(EventLoopPhase::Shutdown, std::memory_order_release);
    }
    if (wakeupChannel_) {
        wakeupChannel_->disableAll();
        wakeupChannel_->remove();
        platform::closeWakeupFds(wakeupFds_);
    }
    detail::ioEngineFromPoller(*poller_).markShutdown();
    t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;

    while (!quit_) {
        if (!hasPendingActiveChannels()) {
            activeChannels_.clear();
            activeChannelCursor_ = 0;
            const int pollTimeout =
                (hasPendingControlSources() || hasPendingLifecycleNodes())
                ? 0
                : timerQueue_->pollTimeoutMs(10000);
            detail::IoNoticeBatch notices(activeChannels_);
            pollReturnTime_ =
                detail::ioEngineFromPoller(*poller_).wait(pollTimeout, notices);
            emitIocpCompletionMetric();
        }
        dispatchActiveChannels();
        doExpiredTimers(gamenet::base::now());
        doControlSources();
        doLifecycleNodes();
        doPendingFunctors(options_.maxFunctorsPerIteration);
    }

    detail::ioEngineFromPoller(*poller_).beginQuiesce();

    {
        std::lock_guard lock(executorState_->mutex);
        executorState_->accepting = false;
        executorState_->drainingAccepted = true;
    }

    // Quiescing keeps consuming backend completions while draining every
    // operation accepted before quit. The zero-timeout poll is essential on
    // IOCP: CancelIoEx publishes completion packets rather than synchronously
    // completing connection-owned operation storage.
    while (true) {
        if (!hasPendingActiveChannels()) {
            activeChannels_.clear();
            activeChannelCursor_ = 0;
            detail::IoNoticeBatch notices(activeChannels_);
            pollReturnTime_ =
                detail::ioEngineFromPoller(*poller_).wait(0, notices);
            emitIocpCompletionMetric();
        }
        dispatchActiveChannels();

        const bool hasPending = hasPendingFunctors();
        const bool hasControl = hasPendingControlSources();
        if (hasControl) {
            doControlSources();
        }
        if (hasPendingLifecycleNodes()) {
            doLifecycleNodes();
        }
        if (hasPending) {
            doPendingFunctors(options_.maxFunctorsPerIteration);
        }

        if (!hasPendingFunctors() &&
            !hasPendingControlSources() &&
            !hasPendingLifecycleNodes() &&
            !hasPendingActiveChannels() &&
            detail::ioEngineFromPoller(*poller_).quiescent()) {
            break;
        }
    }

    {
        std::lock_guard lifecycleLock(lifecycleState_->mutex);
        lifecycleState_->phase = EventLoopPhase::FinalDraining;
        phase_.store(
            EventLoopPhase::FinalDraining,
            std::memory_order_release);
    }

    // Seal the backend/lifecycle quiet point and run one final accepted-work
    // fixed point. A self-rearmed internal callback remains legal, but no
    // external producer can enter after Quiescing linearized.
    while (true) {
        if (hasPendingActiveChannels() ||
            !detail::ioEngineFromPoller(*poller_).quiescent()) {
            if (!hasPendingActiveChannels()) {
                activeChannels_.clear();
                activeChannelCursor_ = 0;
                detail::IoNoticeBatch notices(activeChannels_);
                pollReturnTime_ =
                    detail::ioEngineFromPoller(*poller_).wait(0, notices);
                emitIocpCompletionMetric();
            }
            dispatchActiveChannels();
        }
        if (hasPendingControlSources()) {
            doControlSources();
        }
        if (hasPendingLifecycleNodes()) {
            doLifecycleNodes();
        }
        if (hasPendingFunctors()) {
            doPendingFunctors(options_.maxFunctorsPerIteration);
        }
        if (!hasPendingFunctors() &&
            !hasPendingControlSources() &&
            !hasPendingLifecycleNodes() &&
            !hasPendingActiveChannels() &&
            detail::ioEngineFromPoller(*poller_).quiescent()) {
            break;
        }
    }

    {
        // Publish final-drain completion and both internal admission planes as
        // one state transition. Repeated quit cannot resurrect owner identity.
        std::unique_lock executorLock(executorState_->mutex);
        std::unique_lock controlLock(controlState_->mutex);
        std::unique_lock lifecycleLock(lifecycleState_->mutex);
        executorState_->drainingAccepted = false;
        controlState_->phase = EventLoopControlSource::State::Phase::Shutdown;
        lifecycleState_->phase = EventLoopPhase::Shutdown;
        phase_.store(EventLoopPhase::Shutdown, std::memory_order_release);
    }

    looping_ = false;
}

void EventLoop::quit() {
    const bool crossThread = !isInLoopThread();
    // Keep executor->control->lifecycle lock order shared with final shutdown
    // and destruction so all admission planes observe one transition.
    std::unique_lock executorLock(executorState_->mutex);
    std::unique_lock controlLock(controlState_->mutex);
    std::unique_lock lifecycleLock(lifecycleState_->mutex);
    executorState_->accepting = false;
    if (controlState_->phase == EventLoopControlSource::State::Phase::Shutdown ||
        lifecycleState_->phase == EventLoopPhase::Shutdown) {
        executorState_->drainingAccepted = false;
        quit_.store(true, std::memory_order_relaxed);
        return;
    }
    executorState_->drainingAccepted = true;
    controlState_->phase = EventLoopControlSource::State::Phase::Draining;
    lifecycleState_->phase = EventLoopPhase::Quiescing;
    phase_.store(EventLoopPhase::Quiescing, std::memory_order_release);
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
    PostResult result = PostResult::QueueFull;
    {
        std::lock_guard admissionLock(executorState_->mutex);
        if (!executorState_->accepting) {
            rejectedFunctorCount_.fetch_add(1, std::memory_order_relaxed);
            result = PostResult::Shutdown;
        } else if (tryQueueInLoopImpl(std::move(cb), true)) {
            result = PostResult::Accepted;
        }
    }
    if (result == PostResult::Accepted) {
        return;
    }
    if (result == PostResult::Shutdown) {
        throw std::logic_error("EventLoop pending functor admission is closed");
    }
    throw std::overflow_error("EventLoop pending functor queue is full");
}

bool EventLoop::tryQueueInLoop(Functor cb) {
    std::lock_guard admissionLock(executorState_->mutex);
    if (!executorState_->accepting) {
        rejectedFunctorCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
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

std::pair<std::size_t, gamenet::base::Timestamp>
EventLoop::pendingFunctorLoadSnapshot() const {
    std::lock_guard lock(mutex_);
    if (pendingFunctors_.empty()) {
        return {0, {}};
    }
    return {pendingFunctors_.size(), pendingFunctors_.front().enqueuedAt};
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

EventLoopLifecycleSource EventLoop::attachLifecycleNode(Functor cb) {
    assertInLoopThread();
    if (!cb) {
        throw std::invalid_argument(
            "EventLoop lifecycle node requires a non-empty callback");
    }

    auto callback = std::make_shared<Functor>(std::move(cb));
    auto node = std::make_shared<EventLoopLifecycleSource::Node>();
    node->callback = std::move(callback);

    std::lock_guard lock(lifecycleState_->mutex);
    if (lifecycleState_->phase != EventLoopPhase::Running ||
        lifecycleState_->loop == nullptr) {
        throw std::logic_error(
            "EventLoop lifecycle-node attachment is closed");
    }
    if (lifecycleState_->nodes.size() >= lifecycleState_->maxNodes) {
        throw std::length_error(
            "EventLoop lifecycle-node capacity is exhausted");
    }
    if (lifecycleState_->nextNodeId == 0) {
        throw std::overflow_error(
            "EventLoop lifecycle-node identity is exhausted");
    }

    node->id = lifecycleState_->nextNodeId++;
    lifecycleState_->nodes.emplace(node->id, node);
    ++lifecycleState_->attachedCount;
    return EventLoopLifecycleSource(
        lifecycleState_,
        node,
        node->generation);
}

void EventLoop::detachLifecycleNode(
    const EventLoopLifecycleSource& source) {
    assertInLoopThread();
    const auto state = source.state_.lock();
    const auto node = source.node_.lock();
    if (!state || !node || state.get() != lifecycleState_.get()) {
        return;
    }

    std::shared_ptr<EventLoopLifecycleSource::Node> retired;
    {
        std::lock_guard lock(state->mutex);
        if (!node->attached || node->generation != source.generation_) {
            return;
        }

        node->attached = false;
        --state->attachedCount;
        ++node->generation;
        if (node->generation == 0) {
            ++node->generation;
        }

        // A dirty/active generation owns committed callback work. Keep its
        // node in the registry until the owner-thread drain reaches silence.
        if (!node->dirty && !node->active) {
            const auto found = state->nodes.find(node->id);
            if (found != state->nodes.end() &&
                found->second.get() == node.get()) {
                retired = std::move(found->second);
                state->nodes.erase(found);
            }
        }
    }
}

std::size_t EventLoop::attachedLifecycleNodeCount() const {
    std::lock_guard lock(lifecycleState_->mutex);
    return lifecycleState_->attachedCount;
}

std::size_t EventLoop::pendingLifecycleNodeCount() const {
    std::lock_guard lock(lifecycleState_->mutex);
    return lifecycleState_->pendingCount;
}

std::uint64_t EventLoop::lifecycleSignalCount() const noexcept {
    return lifecycleState_->signals.load(std::memory_order_relaxed);
}

std::uint64_t EventLoop::mergedLifecycleSignalCount() const noexcept {
    return lifecycleState_->mergedSignals.load(std::memory_order_relaxed);
}

std::uint64_t EventLoop::rejectedLifecycleSignalCount() const noexcept {
    return lifecycleState_->rejectedSignals.load(std::memory_order_relaxed);
}

EventLoopPhase EventLoop::phase() const noexcept {
    return phase_.load(std::memory_order_acquire);
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
    return runEvery(interval, std::move(cb), RepeatingTimerOptions{});
}

TimerId EventLoop::runEvery(
    TimerDuration interval,
    Functor cb,
    RepeatingTimerOptions options) {
    if (interval <= TimerDuration::zero()) {
        throw std::invalid_argument("runEvery interval must be positive");
    }
    options.validate();
    return timerQueue_->addTimer(
        std::move(cb),
        gamenet::base::now() + interval,
        interval,
        options);
}

TimerScheduleResult EventLoop::tryRunAt(
    gamenet::base::Timestamp time,
    Functor cb) {
    return timerQueue_->tryAddTimer(std::move(cb), time);
}

TimerScheduleResult EventLoop::tryRunAfter(TimerDuration delay, Functor cb) {
    return timerQueue_->tryAddTimer(
        std::move(cb),
        gamenet::base::now() + delay,
        TimerDuration::zero());
}

TimerScheduleResult EventLoop::tryRunEvery(
    TimerDuration interval,
    Functor cb) {
    return tryRunEvery(interval, std::move(cb), RepeatingTimerOptions{});
}

TimerScheduleResult EventLoop::tryRunEvery(
    TimerDuration interval,
    Functor cb,
    RepeatingTimerOptions options) {
    if (interval <= TimerDuration::zero()) {
        throw std::invalid_argument("tryRunEvery interval must be positive");
    }
    return timerQueue_->tryAddTimer(
        std::move(cb),
        gamenet::base::now() + interval,
        interval,
        options);
}

PostResult EventLoop::tryCancel(TimerId timerId) noexcept {
    return timerQueue_->tryCancel(timerId);
}

void EventLoop::cancel(TimerId timerId) {
    (void)tryCancel(timerId);
}

void EventLoop::wakeup() {
    wakeupCount_.fetch_add(1, std::memory_order_relaxed);
    if (detail::ioEngineFromPoller(*poller_).wakeup()) {
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
    const auto result = detail::ioEngineFromPoller(*poller_).
        registerOrUpdateReadiness(channel);
    if (!detail::accepted(result)) {
        throw std::logic_error(
            "EventLoop::updateChannel I/O Engine rejected registration");
    }
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
    if (!detail::ioEngineFromPoller(*poller_).hasReadiness(channel)) {
        throw std::logic_error(
            "EventLoop::removeChannel registration identity mismatch");
    }

    const auto result =
        detail::ioEngineFromPoller(*poller_).cancelReadiness(channel);
    if (!detail::accepted(result)) {
        throw std::logic_error(
            "EventLoop::removeChannel I/O Engine rejected cancellation");
    }

    if (channel->activeBatchEpoch_ == activeBatchEpoch_) {
        const std::size_t index = channel->activeBatchIndex_;
        if (index < activeChannels_.size() &&
            activeChannels_[index] == channel) {
            activeChannels_[index] = nullptr;
        } else if (eventHandling_) {
            LOG_FATAL << "EventLoop active Channel batch membership is corrupt";
        }
    }
    channel->addedToLoop_ = false;
    channel->advanceRegistrationGeneration();
}

void EventLoop::preserveSocketAssociation(SocketFd sockfd) {
    assertInLoopThread();
    const auto result = detail::ioEngineFromPoller(*poller_).
        commitSocketAssociationPreservation(sockfd);
    if (!ioEngineAcceptedOrUnsupported(result)) {
        throw std::logic_error(
            "EventLoop I/O Engine rejected socket association");
    }
}

void EventLoop::forgetSocketAssociation(SocketFd sockfd) noexcept {
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop socket association rollback used from a different thread";
    }
    const auto result = detail::ioEngineFromPoller(*poller_).
        commitSocketAssociationForget(sockfd);
    if (!ioEngineAcceptedOrUnsupported(result)) {
        LOG_FATAL << "EventLoop I/O Engine rejected socket association rollback";
    }
}

void EventLoop::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    assertInLoopThread();
    const auto result = detail::ioEngineFromPoller(*poller_).
        commitCompletionSubmission(
            operation,
            std::move(lifetime));
    if (!ioEngineAcceptedOrUnsupported(result)) {
        throw std::logic_error(
            "EventLoop I/O Engine rejected completion submission commit");
    }
}

void EventLoop::trackCompletionOperation(void* operation) {
    assertInLoopThread();
    const auto result = detail::ioEngineFromPoller(*poller_).
        commitCompletionCancellation(operation);
    if (!ioEngineAcceptedOrUnsupported(result)) {
        throw std::logic_error(
            "EventLoop I/O Engine rejected completion cancellation commit");
    }
}

bool EventLoop::hasPendingCompletionOperations() const noexcept {
    return !detail::ioEngineFromPoller(*poller_).quiescent();
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return detail::ioEngineFromPoller(*poller_).hasReadiness(channel);
}

void EventLoop::dispatchActiveChannels() {
    assertInLoopThread();
    if (eventHandling_) {
        throw std::logic_error("EventLoop active Channel dispatch cannot re-enter");
    }
    if (!hasPendingActiveChannels()) {
        activeChannels_.clear();
        activeChannelCursor_ = 0;
        return;
    }

    if (activeChannelCursor_ == 0) {
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
    }

    std::size_t drained = 0;
    eventHandling_ = true;
    while (activeChannelCursor_ < activeChannels_.size() &&
           drained < options_.maxActiveChannelsPerIteration) {
        Channel* channel = activeChannels_[activeChannelCursor_++];
        if (channel == nullptr) {
            continue;
        }
        ++drained;
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

    std::size_t remaining = 0;
    for (std::size_t index = activeChannelCursor_;
         index < activeChannels_.size();
         ++index) {
        if (activeChannels_[index] != nullptr) {
            ++remaining;
        }
    }

    EventLoopMetricSample sample;
    sample.event = EventLoopMetricEvent::ActiveChannelsDrained;
    sample.loop = this;
    sample.drainedWork = drained;
    sample.remainingWork = remaining;
    const auto now = gamenet::base::now();
    if (pollReturnTime_ <= now) {
        sample.oldestReadyLatency = now - pollReturnTime_;
    }
    sample.budgetExhausted = remaining != 0;
    emitEventLoopMetric(sample);

    if (remaining == 0) {
        activeChannels_.clear();
        activeChannelCursor_ = 0;
    }
}

bool EventLoop::hasPendingActiveChannels() const noexcept {
    for (std::size_t index = activeChannelCursor_;
         index < activeChannels_.size();
         ++index) {
        if (activeChannels_[index] != nullptr) {
            return true;
        }
    }
    return false;
}

void EventLoop::doExpiredTimers(gamenet::base::Timestamp now) {
    auto result =
        timerQueue_->handleExpired(now, options_.maxTimersPerIteration);
    for (auto& exception : result.exceptions) {
        handleCallbackException(EventLoopCallbackSource::Timer, exception);
    }
    if (result.drained == 0) {
        return;
    }

    EventLoopMetricSample sample;
    sample.event = EventLoopMetricEvent::TimersDrained;
    sample.loop = this;
    sample.drainedWork = result.drained;
    sample.remainingWork = result.remaining;
    sample.oldestReadyLatency = result.oldestReadyLatency;
    sample.budgetExhausted = result.remaining != 0;
    emitEventLoopMetric(sample);
}

void EventLoop::emitIocpCompletionMetric() {
    const auto waitProgress =
        detail::ioEngineFromPoller(*poller_).waitProgress();
    if (waitProgress.wakeupNotices != 0) {
        EventLoopMetricSample wakeupSample;
        wakeupSample.event = EventLoopMetricEvent::WakeupHandled;
        wakeupSample.loop = this;
        wakeupSample.wakeupCount =
            wakeupCount_.load(std::memory_order_relaxed);
        wakeupSample.drainedWork = waitProgress.wakeupNotices;
        emitEventLoopMetric(wakeupSample);
    }

    const auto progress =
        detail::ioEngineFromPoller(*poller_).completionProgress();
    if (progress.drained == 0 && progress.deferred == 0) {
        return;
    }

    EventLoopMetricSample sample;
    sample.event =
        EventLoopMetricEvent::IocpCompletionPacketsDrained;
    sample.loop = this;
    sample.drainedWork = progress.drained;
    // This is the exact user-space deferred remainder. Windows exposes no
    // non-destructive kernel completion-port queue depth; a full dequeue is
    // reported independently through budgetExhausted.
    sample.remainingWork = progress.deferred;
    sample.budgetExhausted = progress.budgetExhausted;
    emitEventLoopMetric(sample);
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

bool EventLoop::hasPendingLifecycleNodes() const {
    std::lock_guard lock(lifecycleState_->mutex);
    return lifecycleState_->pendingCount != 0;
}

bool EventLoop::hasPendingFunctors() const {
    std::lock_guard lock(mutex_);
    return !pendingFunctors_.empty();
}

void EventLoop::doControlSources() {
    assertInLoopThread();

    std::fill(
        controlDrainWords_.begin(),
        controlDrainWords_.end(),
        std::uint64_t{0});
    std::size_t selectedCount = 0;
    std::size_t pendingPeak = 0;
    gamenet::base::Timestamp oldestPending{};
    {
        std::lock_guard lock(controlState_->mutex);
        pendingPeak = controlState_->pendingPeak;
        controlState_->pendingPeak = 0;
        for (std::size_t wordIndex = 0;
             wordIndex < controlState_->pendingWords.size() &&
             selectedCount < options_.maxControlCallbacksPerIteration;
             ++wordIndex) {
            auto available = controlState_->pendingWords[wordIndex];
            while (available != 0 &&
                   selectedCount < options_.maxControlCallbacksPerIteration) {
                const auto bitIndex =
                    static_cast<std::size_t>(std::countr_zero(available));
                const std::uint64_t mask = std::uint64_t{1} << bitIndex;
                const std::size_t slotIndex = wordIndex * 64 + bitIndex;
                available &= available - 1;
                controlState_->pendingWords[wordIndex] &= ~mask;
                controlDrainWords_[wordIndex] |= mask;
                --controlState_->pendingCount;
                ++selectedCount;

                const auto pendingSince =
                    controlState_->slots[slotIndex].pendingSince;
                if (oldestPending == gamenet::base::Timestamp{} ||
                    pendingSince < oldestPending) {
                    oldestPending = pendingSince;
                }
                controlState_->slots[slotIndex].pendingSince = {};
            }
        }
    }

    if (selectedCount == 0) {
        return;
    }

    std::size_t drainedCount = 0;
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

            ++drainedCount;
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
    sample.loop = this;
    sample.pendingControlSources = drainedCount;
    sample.pendingControlSourcePeak =
        (std::max)(pendingPeak, selectedCount);
    sample.wakeupCount = wakeupCount_.load(std::memory_order_relaxed);
    sample.drainedWork = drainedCount;
    sample.remainingWork = pendingControlSourceCount();
    const auto now = gamenet::base::now();
    if (oldestPending != gamenet::base::Timestamp{} &&
        oldestPending <= now) {
        sample.oldestReadyLatency = now - oldestPending;
    }
    sample.budgetExhausted = sample.remainingWork != 0;
    emitEventLoopMetric(sample);

    if (sample.remainingWork != 0) {
        wakeup();
    }
}

void EventLoop::doLifecycleNodes() {
    assertInLoopThread();

    std::size_t roundCount = 0;
    gamenet::base::Timestamp oldestPending{};
    {
        std::lock_guard lock(lifecycleState_->mutex);
        roundCount = std::min(
            options_.maxLifecycleCallbacksPerIteration,
            lifecycleState_->pendingCount);
        if (lifecycleState_->dirtyHead != nullptr) {
            oldestPending = lifecycleState_->dirtyHead->pendingSince;
        }
    }

    std::size_t drainedCount = 0;
    for (std::size_t index = 0; index < roundCount; ++index) {
        std::shared_ptr<EventLoopLifecycleSource::Node> node;
        std::shared_ptr<Functor> callback;
        std::uint64_t callbackGeneration = 0;
        {
            std::lock_guard lock(lifecycleState_->mutex);
            auto* dirty = lifecycleState_->dirtyHead;
            if (dirty == nullptr) {
                break;
            }

            lifecycleState_->dirtyHead = dirty->dirtyNext;
            if (lifecycleState_->dirtyHead == nullptr) {
                lifecycleState_->dirtyTail = nullptr;
            }
            dirty->dirtyNext = nullptr;
            dirty->dirty = false;
            --lifecycleState_->pendingCount;

            const auto found =
                lifecycleState_->nodes.find(dirty->id);
            if (found == lifecycleState_->nodes.end() ||
                found->second.get() != dirty) {
                LOG_FATAL << "EventLoop lifecycle dirty-set identity is corrupt";
            }

            node = found->second;
            callback = node->callback;
            callbackGeneration = node->pendingGeneration;
            node->pendingGeneration = 0;
            node->pendingSince = {};
            node->active = true;
            lifecycleState_->activeNode = node.get();
            lifecycleState_->activeGeneration = callbackGeneration;
        }

        std::exception_ptr exception;
        try {
            (*callback)();
        } catch (...) {
            exception = std::current_exception();
        }

        {
            std::shared_ptr<EventLoopLifecycleSource::Node> retired;
            {
                std::lock_guard lock(lifecycleState_->mutex);
                node->active = false;
                if (lifecycleState_->activeNode == node.get()) {
                    lifecycleState_->activeNode = nullptr;
                    lifecycleState_->activeGeneration = 0;
                }
                if (!node->attached && !node->dirty) {
                    const auto found =
                        lifecycleState_->nodes.find(node->id);
                    if (found != lifecycleState_->nodes.end() &&
                        found->second.get() == node.get()) {
                        retired = std::move(found->second);
                        lifecycleState_->nodes.erase(found);
                    }
                }
            }
        }

        if (exception) {
            handleCallbackException(
                EventLoopCallbackSource::Lifecycle,
                exception);
        }
        ++drainedCount;
    }

    const auto remaining = pendingLifecycleNodeCount();
    if (drainedCount != 0) {
        EventLoopMetricSample sample;
        sample.event = EventLoopMetricEvent::LifecycleNodesDrained;
        sample.loop = this;
        sample.drainedWork = drainedCount;
        sample.remainingWork = remaining;
        const auto now = gamenet::base::now();
        if (oldestPending != gamenet::base::Timestamp{} &&
            oldestPending <= now) {
            sample.oldestReadyLatency = now - oldestPending;
        }
        sample.budgetExhausted = remaining != 0;
        emitEventLoopMetric(sample);
    }

    if (remaining != 0) {
        // The remaining intrusive entries are already allocated and linked.
        // A wakeup requests another fair loop turn without queue admission.
        wakeup();
    }
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
        sample.drainedWork = functors.size();
        sample.remainingWork = pendingFunctorCount();
        sample.oldestReadyLatency = sample.oldestPendingLatency;
        sample.budgetExhausted = sample.remainingWork != 0;
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
