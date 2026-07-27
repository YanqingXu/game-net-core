#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerId.h"
#include "detail/EventLoopLifecycleRegistry.h"

#include "gamenet/core/base/Logger.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gamenet::net {

void TcpServerStopOptions::validate() const {
    if (drainTimeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("TcpServer drain timeout must not be negative");
    }
}

void TcpServerAdmissionOptions::validate() const {
    if (maxConnectionAttemptsPerPeerPerWindow > 0 &&
        connectionAttemptWindow <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "TcpServer connection-attempt window must be positive when rate limiting is enabled");
    }
    if (maxConnectionAttemptsPerPeerPerWindow > 0 &&
        maxTrackedPeerAddresses == 0) {
        throw std::invalid_argument(
            "TcpServer tracked-peer capacity must be positive when rate limiting is enabled");
    }
    if (unauthenticatedTimeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "TcpServer unauthenticated timeout must not be negative");
    }
}

struct TcpServer::GracefulStopState {
    GracefulStopState()
        : future(promise.get_future().share()) {}

    bool complete(TcpServerStopResult result) {
        std::lock_guard lock(mutex);
        if (completed) {
            return false;
        }
        completed = true;
        promise.set_value(result);
        return true;
    }

    bool isCompleted() const {
        std::lock_guard lock(mutex);
        return completed;
    }

    std::promise<TcpServerStopResult> promise;
    TcpServerStopFuture future;
    mutable std::mutex mutex;
    bool completed{false};
    bool begun{false};
    bool forceStarted{false};
    std::size_t initialConnectionCount{0};
    std::size_t forcedConnectionCount{0};
    TcpServerStopOptions options;
    TimerId timeoutTimer;
};

struct TcpServer::WorkerStopParticipant {
    enum class Command {
        None,
        Graceful,
        Force,
    };

    explicit WorkerStopParticipant(
        EventLoop* loopValue,
        EventLoopLifecycleSource baseSourceValue)
        : loop(loopValue),
          baseSource(std::move(baseSourceValue)) {}

    EventLoop* loop;
    EventLoopLifecycleSource source;
    EventLoopLifecycleSource baseSource;
    std::mutex mutex;
    std::uint64_t generation{0};
    Command command{Command::None};
    bool callbacksInstalled{false};
    bool forceIssued{false};
    bool locallyQuiet{false};
    bool quietReported{false};
    bool baseReleased{false};
    bool ackReported{false};
    std::vector<TcpConnectionPtr> connections;
    std::vector<std::string> connectionNames;
    std::unordered_set<std::string> pendingConnectionNames;
};

struct TcpServer::AggregateStopState {
    std::uint64_t generation{0};
    TcpServerStopOutcome outcome{TcpServerStopOutcome::Drained};
    bool forceStarted{false};
    bool joined{false};
    std::shared_ptr<GracefulStopState> gracefulState;
    std::vector<std::shared_ptr<WorkerStopParticipant>> participants;
};

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, reusePort)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_)) {
    acceptor_->setNewConnectionCallback(
        [this](SocketFd sockfd, const InetAddress& peerAddr) { newConnection(sockfd, peerAddr); });

    std::weak_ptr<void> lifetime = lifetimeToken_;
    auto source = detail::EventLoopLifecycleRegistry::attach(
        *loop_,
        [this, lifetime] {
            if (lifetime.lock()) {
                driveStopLifecycleInLoop();
            }
        });
    stopLifecycleSource_ =
        std::make_shared<EventLoopLifecycleSource>(std::move(source));
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    const auto gracefulState = gracefulStopStateSnapshot();
    if (gracefulState && !gracefulState->isCompleted()) {
        gracefulState->complete(TcpServerStopResult{
            .outcome = TcpServerStopOutcome::ServerDestroyed,
            .initialConnectionCount = gracefulState->initialConnectionCount,
            .forcedConnectionCount = connections_.size(),
        });
    }
    lifetimeToken_.reset();
    acceptor_->setNewConnectionCallback({});
    if (acceptor_->listening()) {
        acceptor_->stop();
    }
    if (!forceCloseAllConnections()) {
        threadPool_->stop();
    }
    for (const auto& participant : workerStopParticipants_) {
        if (participant && participant->loop == loop_) {
            detail::EventLoopLifecycleRegistry::detach(
                *loop_,
                participant->source);
        }
    }
    std::shared_ptr<EventLoopLifecycleSource> stopSource;
    {
        std::lock_guard lock(stopLifecycleMutex_);
        stopSource = std::move(stopLifecycleSource_);
    }
    if (stopSource) {
        detail::EventLoopLifecycleRegistry::detach(*loop_, *stopSource);
    }
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::setThreadInitCallback(ThreadInitCallback cb) {
    threadInitCallback_ = std::move(cb);
}

void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpServer::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpServer::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpServer::setCloseInfoCallback(CloseInfoCallback cb) {
    closeInfoCallback_ = std::move(cb);
}

void TcpServer::setConnectionBackpressureOptions(
    TcpConnectionBackpressureOptions options) {
    options.validate();
    if (started_.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "TcpServer backpressure options must be configured before start");
    }
    backpressureOptions_ = options;
}

void TcpServer::setAcceptErrorCallback(AcceptorErrorCallback cb) {
    acceptErrorCallback_ = std::move(cb);
    acceptor_->setErrorCallback(acceptErrorCallback_);
}

void TcpServer::setCallbackExceptionHandler(
    TcpConnectionCallbackExceptionHandler cb) {
    callbackExceptionHandler_ = std::move(cb);
}

void TcpServer::setAdmissionOptions(TcpServerAdmissionOptions options) {
    options.validate();
    if (started_.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "TcpServer admission options must be configured before start");
    }
    admissionOptions_ = options;
}

void TcpServer::setAdmissionMetricCallback(
    TcpServerAdmissionMetricCallback cb) {
    if (started_.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "TcpServer admission metric callback must be configured before start");
    }
    admissionMetricCallback_ = std::move(cb);
}

bool TcpServer::tryMarkConnectionAuthenticated(
    const TcpConnectionPtr& connection) {
    if (!connection) {
        return false;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    auto mark = [this, lifetime, connection] {
        if (lifetime.lock()) {
            markConnectionAuthenticatedInLoop(connection);
        }
    };
    if (loop_->isInLoopThread()) {
        mark();
        return true;
    }
    return loop_->executor().tryQueue(std::move(mark));
}

TcpServerAdmissionStats TcpServer::admissionStats() const noexcept {
    return TcpServerAdmissionStats{
        .accepted = acceptedConnections_.load(std::memory_order_relaxed),
        .rejectedConnectionLimit =
            rejectedConnectionLimit_.load(std::memory_order_relaxed),
        .rejectedPerPeerLimit =
            rejectedPerPeerLimit_.load(std::memory_order_relaxed),
        .rejectedPerPeerRateLimit =
            rejectedPerPeerRateLimit_.load(std::memory_order_relaxed),
        .rejectedPeerTrackingCapacity =
            rejectedPeerTrackingCapacity_.load(std::memory_order_relaxed),
        .authenticated =
            authenticatedConnections_.load(std::memory_order_relaxed),
        .authenticationTimedOut =
            authenticationTimeouts_.load(std::memory_order_relaxed),
        .activeConnections =
            activeAdmissionConnections_.load(std::memory_order_relaxed),
    };
}

const InetAddress& TcpServer::listenAddress() const noexcept {
    return acceptor_->listenAddress();
}

std::size_t TcpServer::connectionCount() const {
    loop_->assertInLoopThread();
    if (aggregateStopState_ && aggregateStopState_->forceStarted) {
        // Compatibility view: immediate/forced stop logically detaches every
        // connection at commit time. Internal ownership remains in the base
        // map until worker cleanup is quiet and BaseReleased can be published.
        return 0;
    }
    return connections_.size();
}

void TcpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    stopped_ = false;
    try {
        workerStopParticipants_.clear();
        std::weak_ptr<void> lifetime = lifetimeToken_;
        const auto userThreadInit = threadInitCallback_;
        threadPool_->start(
            [this, lifetime, userThreadInit](EventLoop* workerLoop) {
                if (!lifetime.lock()) {
                    return;
                }
                registerWorkerStopParticipant(workerLoop);
                if (userThreadInit) {
                    userThreadInit(workerLoop);
                }
            });
    } catch (...) {
        workerStopParticipants_.clear();
        started_.store(false, std::memory_order_relaxed);
        stopped_ = true;
        throw;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            acceptor_->listen();
        }
    });
}

void TcpServer::stop() {
    immediateStopRequested_.store(true, std::memory_order_release);
    if (loop_->isInLoopThread()) {
        driveStopLifecycleInLoop();
        return;
    }
    (void)signalStopLifecycle();
}

TcpServerStopFuture TcpServer::stopGracefully(TcpServerStopOptions options) {
    options.validate();

    std::shared_ptr<GracefulStopState> state;
    {
        std::lock_guard lock(gracefulStopMutex_);
        if (gracefulStopState_) {
            return gracefulStopState_->future;
        }
        state = std::make_shared<GracefulStopState>();
        state->options = options;
        gracefulStopState_ = state;
    }

    if (loop_->isInLoopThread()) {
        driveStopLifecycleInLoop();
        return state->future;
    }

    const PostResult admitted = signalStopLifecycle();
    if (admitted != PostResult::Accepted) {
        state->complete(TcpServerStopResult{
            .outcome = TcpServerStopOutcome::SchedulingFailed,
        });
        std::lock_guard lock(gracefulStopMutex_);
        if (gracefulStopState_ == state) {
            gracefulStopState_.reset();
        }
    }
    return state->future;
}

PostResult TcpServer::signalStopLifecycle() noexcept {
    std::shared_ptr<EventLoopLifecycleSource> source;
    {
        std::lock_guard lock(stopLifecycleMutex_);
        source = stopLifecycleSource_;
    }
    if (!source) {
        return PostResult::OwnerUnavailable;
    }
    return source->signal();
}

void TcpServer::registerWorkerStopParticipant(EventLoop* workerLoop) {
    workerLoop->assertInLoopThread();

    std::shared_ptr<EventLoopLifecycleSource> baseSource;
    {
        std::lock_guard lock(stopLifecycleMutex_);
        baseSource = stopLifecycleSource_;
    }
    if (!baseSource) {
        throw std::logic_error(
            "TcpServer base lifecycle source is unavailable");
    }

    auto participant =
        std::make_shared<WorkerStopParticipant>(
            workerLoop,
            *baseSource);
    const auto source = detail::EventLoopLifecycleRegistry::attach(
        *workerLoop,
        [participant] {
            driveWorkerStopParticipant(participant);
        });
    participant->source = source;
    workerStopParticipants_.push_back(std::move(participant));
}

void TcpServer::driveStopLifecycleInLoop() {
    loop_->assertInLoopThread();

    const auto gracefulState = gracefulStopStateSnapshot();
    if (gracefulState &&
        !gracefulState->isCompleted() &&
        !gracefulState->begun) {
        gracefulState->begun = true;
        beginGracefulStopInLoop(
            gracefulState,
            gracefulState->options);
    }

    if (immediateStopRequested_.exchange(
            false,
            std::memory_order_acq_rel)) {
        stopInLoop();
    }

    consumeWorkerStopNotificationsInLoop();
}

void TcpServer::beginAggregateStopInLoop(
    const std::shared_ptr<GracefulStopState>& gracefulState,
    TcpServerStopOutcome outcome,
    bool force) {
    loop_->assertInLoopThread();
    if (aggregateStopState_) {
        if (force) {
            requestAggregateForceInLoop(outcome);
        }
        return;
    }

    stopped_ = true;
    acceptor_->setNewConnectionCallback({});
    if (acceptor_->listening()) {
        acceptor_->stop();
    }

    if (nextStopGeneration_ == 0) {
        throw std::overflow_error(
            "TcpServer stop generation is exhausted");
    }
    auto aggregate = std::make_shared<AggregateStopState>();
    aggregate->generation = nextStopGeneration_++;
    aggregate->outcome = outcome;
    aggregate->forceStarted = force;
    aggregate->gracefulState = gracefulState;
    aggregate->participants = workerStopParticipants_;
    aggregateStopState_ = aggregate;

    std::unordered_map<EventLoop*, std::vector<TcpConnectionPtr>>
        connectionsByLoop;
    for (const auto& [name, connection] : connections_) {
        (void)name;
        connectionsByLoop[connection->getLoop()].push_back(connection);
    }

    for (const auto& participant : aggregate->participants) {
        std::vector<TcpConnectionPtr> assigned;
        const auto found = connectionsByLoop.find(participant->loop);
        if (found != connectionsByLoop.end()) {
            assigned = std::move(found->second);
            connectionsByLoop.erase(found);
        }

        {
            std::lock_guard lock(participant->mutex);
            participant->generation = aggregate->generation;
            participant->command =
                force
                ? WorkerStopParticipant::Command::Force
                : WorkerStopParticipant::Command::Graceful;
            participant->callbacksInstalled = false;
            participant->forceIssued = false;
            participant->locallyQuiet = assigned.empty();
            participant->quietReported = false;
            participant->baseReleased = false;
            participant->ackReported = false;
            participant->connections = std::move(assigned);
            participant->connectionNames.clear();
            participant->pendingConnectionNames.clear();
            participant->connectionNames.reserve(
                participant->connections.size());
            participant->pendingConnectionNames.reserve(
                participant->connections.size());
            for (const auto& connection : participant->connections) {
                participant->connectionNames.push_back(
                    connection->name());
                participant->pendingConnectionNames.insert(
                    connection->name());
            }
        }

        const PostResult signaled = participant->source.signal();
        if (signaled != PostResult::Accepted) {
            LOG_ERROR
                << "TcpServer worker aggregate rejected stop generation "
                << aggregate->generation;
        }
    }

    if (!connectionsByLoop.empty()) {
        LOG_FATAL
            << "TcpServer has connections without a worker stop participant";
    }

    if (aggregate->participants.empty()) {
        finishAggregateStopInLoop();
    }
}

void TcpServer::requestAggregateForceInLoop(
    TcpServerStopOutcome outcome) {
    loop_->assertInLoopThread();
    const auto aggregate = aggregateStopState_;
    if (!aggregate || aggregate->joined) {
        return;
    }

    if (outcome == TcpServerStopOutcome::ForcedByImmediateStop ||
        !aggregate->forceStarted) {
        aggregate->outcome = outcome;
    }
    if (aggregate->forceStarted) {
        return;
    }
    aggregate->forceStarted = true;
    if (aggregate->gracefulState) {
        aggregate->gracefulState->forceStarted = true;
        aggregate->gracefulState->forcedConnectionCount =
            connections_.size();
    }

    for (const auto& participant : aggregate->participants) {
        bool signal = false;
        {
            std::lock_guard lock(participant->mutex);
            if (participant->generation != aggregate->generation ||
                participant->ackReported) {
                continue;
            }
            if (participant->command !=
                WorkerStopParticipant::Command::Force) {
                participant->command =
                    WorkerStopParticipant::Command::Force;
                signal = true;
            }
        }
        if (signal) {
            (void)participant->source.signal();
        }
    }
}

void TcpServer::workerConnectionClosed(
    const std::shared_ptr<WorkerStopParticipant>& participant,
    std::uint64_t generation,
    const TcpConnectionPtr& connection) {
    participant->loop->assertInLoopThread();
    connection->connectDestroyed();

    bool notifyBase = false;
    {
        std::lock_guard lock(participant->mutex);
        if (participant->generation != generation ||
            participant->ackReported) {
            return;
        }
        if (participant->pendingConnectionNames.erase(
                connection->name()) == 0) {
            return;
        }
        if (participant->pendingConnectionNames.empty()) {
            participant->locallyQuiet = true;
            if (!participant->quietReported) {
                participant->quietReported = true;
                notifyBase = true;
            }
        }
    }
    if (notifyBase) {
        (void)participant->baseSource.signal();
    }
}

void TcpServer::driveWorkerStopParticipant(
    const std::shared_ptr<WorkerStopParticipant>& participant) {
    participant->loop->assertInLoopThread();

    std::uint64_t generation = 0;
    WorkerStopParticipant::Command initialCommand =
        WorkerStopParticipant::Command::None;
    std::vector<TcpConnectionPtr> installConnections;
    std::vector<TcpConnectionPtr> forceConnections;
    bool notifyBaseQuiet = false;
    bool notifyBaseAck = false;
    bool detach = false;

    {
        std::lock_guard lock(participant->mutex);
        generation = participant->generation;
        if (generation == 0) {
            return;
        }

        if (!participant->callbacksInstalled) {
            participant->callbacksInstalled = true;
            initialCommand = participant->command;
            installConnections = participant->connections;
            if (initialCommand ==
                WorkerStopParticipant::Command::Force) {
                participant->forceIssued = true;
            }
        } else if (
            participant->command ==
                WorkerStopParticipant::Command::Force &&
            !participant->forceIssued) {
            participant->forceIssued = true;
            forceConnections = participant->connections;
        }

        if (participant->pendingConnectionNames.empty()) {
            participant->locallyQuiet = true;
            if (!participant->quietReported) {
                participant->quietReported = true;
                notifyBaseQuiet = true;
            }
        }

        if (participant->baseReleased &&
            participant->locallyQuiet &&
            !participant->ackReported) {
            participant->ackReported = true;
            participant->connections.clear();
            participant->connectionNames.clear();
            participant->pendingConnectionNames.clear();
            notifyBaseAck = true;
            detach = true;
        }
    }

    for (const auto& connection : installConnections) {
        std::weak_ptr<WorkerStopParticipant> weakParticipant =
            participant;
        connection->setCloseCallback(
            [weakParticipant, generation](
                const TcpConnectionPtr& closedConnection) {
                if (const auto locked = weakParticipant.lock()) {
                    workerConnectionClosed(
                        locked,
                        generation,
                        closedConnection);
                } else {
                    closedConnection->connectDestroyed();
                }
            });

        if (connection->disconnected()) {
            workerConnectionClosed(
                participant,
                generation,
                connection);
        } else if (
            initialCommand ==
            WorkerStopParticipant::Command::Force) {
            (void)connection->tryForceClose();
        } else {
            (void)connection->tryShutdown();
        }
    }

    for (const auto& connection : forceConnections) {
        if (!connection->disconnected()) {
            (void)connection->tryForceClose();
        }
    }

    // Synchronous force-close callbacks may have transitioned the participant
    // to quiet after the initial snapshot.
    {
        std::lock_guard lock(participant->mutex);
        if (participant->generation == generation &&
            participant->pendingConnectionNames.empty()) {
            participant->locallyQuiet = true;
            if (!participant->quietReported) {
                participant->quietReported = true;
                notifyBaseQuiet = true;
            }
        }
    }

    if (notifyBaseQuiet || notifyBaseAck) {
        (void)participant->baseSource.signal();
    }
    if (detach) {
        detail::EventLoopLifecycleRegistry::detach(
            *participant->loop,
            participant->source);
    }
}

void TcpServer::releaseWorkerConnectionsInLoop(
    const std::shared_ptr<WorkerStopParticipant>& participant,
    std::uint64_t generation) {
    loop_->assertInLoopThread();

    std::vector<std::string> connectionNames;
    {
        std::lock_guard lock(participant->mutex);
        if (participant->generation != generation ||
            !participant->locallyQuiet ||
            participant->baseReleased) {
            return;
        }
        connectionNames = participant->connectionNames;
    }

    for (const auto& connectionName : connectionNames) {
        if (connections_.erase(connectionName) != 0) {
            releaseConnectionAdmission(connectionName);
        }
    }

    {
        std::lock_guard lock(participant->mutex);
        if (participant->generation != generation ||
            participant->baseReleased) {
            return;
        }
        participant->baseReleased = true;
    }
    (void)participant->source.signal();
}

void TcpServer::consumeWorkerStopNotificationsInLoop() {
    loop_->assertInLoopThread();
    const auto aggregate = aggregateStopState_;
    if (!aggregate || aggregate->joined) {
        return;
    }

    for (const auto& participant : aggregate->participants) {
        releaseWorkerConnectionsInLoop(
            participant,
            aggregate->generation);
    }

    bool allAcknowledged = true;
    for (const auto& participant : aggregate->participants) {
        std::lock_guard lock(participant->mutex);
        if (participant->generation != aggregate->generation ||
            !participant->ackReported) {
            allAcknowledged = false;
            break;
        }
    }
    if (allAcknowledged) {
        finishAggregateStopInLoop();
    }
}

void TcpServer::finishAggregateStopInLoop() {
    loop_->assertInLoopThread();
    const auto aggregate = aggregateStopState_;
    if (!aggregate || aggregate->joined) {
        return;
    }
    aggregate->joined = true;

    if (aggregate->gracefulState &&
        aggregate->gracefulState->timeoutTimer.valid()) {
        loop_->cancel(aggregate->gracefulState->timeoutTimer);
        aggregate->gracefulState->timeoutTimer = {};
    }

    clearConnectionAdmission();
    threadPool_->stop();
    if (aggregate->gracefulState) {
        aggregate->gracefulState->complete(TcpServerStopResult{
            .outcome = aggregate->outcome,
            .initialConnectionCount =
                aggregate->gracefulState->initialConnectionCount,
            .forcedConnectionCount =
                aggregate->gracefulState->forcedConnectionCount,
        });
    }
    aggregateStopState_.reset();
}

void TcpServer::stopInLoop() {
    loop_->assertInLoopThread();
    if (aggregateStopState_) {
        requestAggregateForceInLoop(
            TcpServerStopOutcome::ForcedByImmediateStop);
        return;
    }
    if (stopped_) {
        return;
    }
    beginAggregateStopInLoop(
        {},
        TcpServerStopOutcome::ForcedByImmediateStop,
        true);
}

void TcpServer::beginGracefulStopInLoop(
    const std::shared_ptr<GracefulStopState>& state,
    TcpServerStopOptions options) {
    loop_->assertInLoopThread();
    if (state->isCompleted()) {
        return;
    }
    if (stopped_) {
        state->complete(TcpServerStopResult{
            .outcome = TcpServerStopOutcome::AlreadyStopped,
        });
        return;
    }

    state->initialConnectionCount = connections_.size();
    beginAggregateStopInLoop(
        state,
        TcpServerStopOutcome::Drained,
        false);
    if (state->isCompleted()) {
        return;
    }

    if (options.drainTimeout == std::chrono::milliseconds::zero()) {
        forceGracefulStopInLoop(state, TcpServerStopOutcome::ForcedAfterTimeout);
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    state->timeoutTimer = loop_->runAfter(options.drainTimeout, [this, lifetime, state] {
        state->timeoutTimer = {};
        if (!lifetime.lock()) {
            state->complete(TcpServerStopResult{
                .outcome = TcpServerStopOutcome::ServerDestroyed,
                .initialConnectionCount = state->initialConnectionCount,
                .forcedConnectionCount = state->forcedConnectionCount,
            });
            return;
        }
        forceGracefulStopInLoop(state, TcpServerStopOutcome::ForcedAfterTimeout);
    });
}

void TcpServer::forceGracefulStopInLoop(
    const std::shared_ptr<GracefulStopState>& state,
    TcpServerStopOutcome outcome) {
    loop_->assertInLoopThread();
    if (state->isCompleted() || state->forceStarted) {
        return;
    }
    state->forceStarted = true;
    if (state->timeoutTimer.valid()) {
        loop_->cancel(state->timeoutTimer);
        state->timeoutTimer = {};
    }
    state->forcedConnectionCount = connections_.size();
    requestAggregateForceInLoop(outcome);
}

void TcpServer::finishGracefulStopInLoop(
    const std::shared_ptr<GracefulStopState>& state,
    TcpServerStopOutcome outcome) {
    loop_->assertInLoopThread();
    if (state->isCompleted()) {
        return;
    }
    if (state->timeoutTimer.valid()) {
        loop_->cancel(state->timeoutTimer);
        state->timeoutTimer = {};
    }
    if (aggregateStopState_ &&
        aggregateStopState_->gracefulState == state) {
        aggregateStopState_->outcome = outcome;
        return;
    }
    threadPool_->stop();
    state->complete(TcpServerStopResult{
        .outcome = outcome,
        .initialConnectionCount = state->initialConnectionCount,
        .forcedConnectionCount = state->forcedConnectionCount,
    });
}

std::shared_ptr<TcpServer::GracefulStopState> TcpServer::gracefulStopStateSnapshot() const {
    std::lock_guard lock(gracefulStopMutex_);
    return gracefulStopState_;
}

void TcpServer::prunePeerRateBuckets(
    std::chrono::steady_clock::time_point now) {
    loop_->assertInLoopThread();
    while (!peerRateExpiries_.empty() &&
           peerRateExpiries_.front().expiresAt <= now) {
        PeerRateExpiry expiry = std::move(peerRateExpiries_.front());
        peerRateExpiries_.pop_front();
        const auto bucket = peerRateBuckets_.find(expiry.peerAddress);
        if (bucket != peerRateBuckets_.end() &&
            bucket->second.generation == expiry.generation &&
            bucket->second.expiresAt <= now) {
            peerRateBuckets_.erase(bucket);
        }
    }
}

bool TcpServer::admitPeer(const std::string& peerAddress) {
    loop_->assertInLoopThread();

    if (admissionOptions_.maxConnectionAttemptsPerPeerPerWindow > 0) {
        const auto now = std::chrono::steady_clock::now();
        prunePeerRateBuckets(now);

        auto bucket = peerRateBuckets_.find(peerAddress);
        if (bucket != peerRateBuckets_.end() && bucket->second.expiresAt <= now) {
            peerRateBuckets_.erase(bucket);
            bucket = peerRateBuckets_.end();
        }

        if (bucket == peerRateBuckets_.end()) {
            if (peerRateBuckets_.size() >=
                admissionOptions_.maxTrackedPeerAddresses) {
                rejectedPeerTrackingCapacity_.fetch_add(
                    1, std::memory_order_relaxed);
                emitAdmissionMetric(
                    TcpServerAdmissionEvent::RejectedPeerTrackingCapacity,
                    peerAddress);
                return false;
            }

            const auto expiresAt =
                now + admissionOptions_.connectionAttemptWindow;
            const std::uint64_t generation = nextPeerRateGeneration_++;
            peerRateBuckets_.emplace(
                peerAddress,
                PeerRateBucket{
                    .expiresAt = expiresAt,
                    .attempts = 1,
                    .generation = generation,
                });
            peerRateExpiries_.push_back(PeerRateExpiry{
                .expiresAt = expiresAt,
                .peerAddress = peerAddress,
                .generation = generation,
            });
        } else {
            if (bucket->second.attempts >=
                admissionOptions_.maxConnectionAttemptsPerPeerPerWindow) {
                rejectedPerPeerRateLimit_.fetch_add(
                    1, std::memory_order_relaxed);
                emitAdmissionMetric(
                    TcpServerAdmissionEvent::RejectedPerPeerRateLimit,
                    peerAddress);
                return false;
            }
            ++bucket->second.attempts;
        }
    }

    if (admissionOptions_.maxConnections > 0 &&
        connections_.size() >= admissionOptions_.maxConnections) {
        rejectedConnectionLimit_.fetch_add(1, std::memory_order_relaxed);
        emitAdmissionMetric(
            TcpServerAdmissionEvent::RejectedConnectionLimit,
            peerAddress);
        return false;
    }

    const auto peerCount = activeConnectionsByPeer_.find(peerAddress);
    const std::size_t activeForPeer =
        peerCount == activeConnectionsByPeer_.end() ? 0 : peerCount->second;
    if (admissionOptions_.maxConnectionsPerPeer > 0 &&
        activeForPeer >= admissionOptions_.maxConnectionsPerPeer) {
        rejectedPerPeerLimit_.fetch_add(1, std::memory_order_relaxed);
        emitAdmissionMetric(
            TcpServerAdmissionEvent::RejectedPerPeerLimit,
            peerAddress);
        return false;
    }

    return true;
}

void TcpServer::trackAcceptedConnection(
    const TcpConnectionPtr& connection,
    const std::string& peerAddress) {
    loop_->assertInLoopThread();
    const std::string& connectionName = connection->name();
    peerByConnection_[connectionName] = peerAddress;
    ++activeConnectionsByPeer_[peerAddress];
    acceptedConnections_.fetch_add(1, std::memory_order_relaxed);
    activeAdmissionConnections_.fetch_add(1, std::memory_order_relaxed);

    if (admissionOptions_.unauthenticatedTimeout >
        std::chrono::milliseconds::zero()) {
        std::weak_ptr<void> lifetime = lifetimeToken_;
        std::weak_ptr<TcpConnection> weakConnection = connection;
        authenticationTimers_[connectionName] = loop_->runAfter(
            admissionOptions_.unauthenticatedTimeout,
            [this, lifetime, weakConnection, connectionName, peerAddress] {
                if (!lifetime.lock()) {
                    return;
                }
                const auto timer = authenticationTimers_.find(connectionName);
                if (timer == authenticationTimers_.end()) {
                    return;
                }
                authenticationTimers_.erase(timer);

                const auto current = connections_.find(connectionName);
                const auto connection = weakConnection.lock();
                if (current == connections_.end() || !connection ||
                    current->second != connection) {
                    return;
                }

                authenticationTimeouts_.fetch_add(1, std::memory_order_relaxed);
                emitAdmissionMetric(
                    TcpServerAdmissionEvent::AuthenticationTimedOut,
                    peerAddress,
                    connectionName);
                connection->forceClose();
            });
    }

    emitAdmissionMetric(
        TcpServerAdmissionEvent::Accepted,
        peerAddress,
        connectionName);
}

void TcpServer::markConnectionAuthenticatedInLoop(
    const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    const auto current = connections_.find(connection->name());
    if (current == connections_.end() || current->second != connection) {
        return;
    }

    const auto timer = authenticationTimers_.find(connection->name());
    if (timer == authenticationTimers_.end()) {
        return;
    }
    loop_->cancel(timer->second);
    authenticationTimers_.erase(timer);

    authenticatedConnections_.fetch_add(1, std::memory_order_relaxed);
    const auto peer = peerByConnection_.find(connection->name());
    emitAdmissionMetric(
        TcpServerAdmissionEvent::Authenticated,
        peer == peerByConnection_.end() ? std::string{} : peer->second,
        connection->name());
}

void TcpServer::releaseConnectionAdmission(
    const std::string& connectionName) {
    loop_->assertInLoopThread();
    const auto timer = authenticationTimers_.find(connectionName);
    if (timer != authenticationTimers_.end()) {
        loop_->cancel(timer->second);
        authenticationTimers_.erase(timer);
    }

    const auto peer = peerByConnection_.find(connectionName);
    if (peer == peerByConnection_.end()) {
        return;
    }
    const auto peerCount = activeConnectionsByPeer_.find(peer->second);
    if (peerCount != activeConnectionsByPeer_.end()) {
        if (peerCount->second <= 1) {
            activeConnectionsByPeer_.erase(peerCount);
        } else {
            --peerCount->second;
        }
    }
    peerByConnection_.erase(peer);
    activeAdmissionConnections_.fetch_sub(1, std::memory_order_relaxed);
}

void TcpServer::clearConnectionAdmission() {
    loop_->assertInLoopThread();
    for (const auto& [name, timer] : authenticationTimers_) {
        (void)name;
        loop_->cancel(timer);
    }
    authenticationTimers_.clear();
    peerByConnection_.clear();
    activeConnectionsByPeer_.clear();
    peerRateBuckets_.clear();
    peerRateExpiries_.clear();
    activeAdmissionConnections_.store(0, std::memory_order_relaxed);
}

void TcpServer::emitAdmissionMetric(
    TcpServerAdmissionEvent event,
    const std::string& peerAddress,
    const std::string& connectionName) noexcept {
    if (!admissionMetricCallback_) {
        return;
    }

    std::size_t activePeerConnections = 0;
    const auto peer = activeConnectionsByPeer_.find(peerAddress);
    if (peer != activeConnectionsByPeer_.end()) {
        activePeerConnections = peer->second;
    }
    try {
        admissionMetricCallback_(TcpServerAdmissionMetric{
            .event = event,
            .peerAddress = peerAddress,
            .connectionName = connectionName,
            .activeConnections = connections_.size(),
            .activePeerConnections = activePeerConnections,
        });
    } catch (const std::exception& error) {
        LOG_ERROR << "TcpServer admission metric callback threw: "
                  << error.what();
    } catch (...) {
        LOG_ERROR << "TcpServer admission metric callback threw a non-standard exception";
    }
}

void TcpServer::newConnection(SocketFd sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();
    Socket pendingSocket(sockfd);
    if (stopped_) {
        return;
    }

    const std::string peerAddress = peerAddr.toIp();
    if (!admitPeer(peerAddress)) {
        return;
    }

    EventLoop* ioLoop = threadPool_->getNextLoop();
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(pendingSocket.fd(), &localStorage)) {
        const int error = sockets::lastError();
        AcceptorErrorAction action = AcceptorErrorAction::Retry;
        if (acceptErrorCallback_) {
            try {
                action = acceptErrorCallback_(AcceptorError{
                    .stage = AcceptorErrorStage::AcceptedSocketSetup,
                    .errorCode = error,
                });
            } catch (const std::exception& callbackError) {
                LOG_ERROR << "TcpServer accept error callback threw: "
                          << callbackError.what();
                action = AcceptorErrorAction::Stop;
            } catch (...) {
                LOG_ERROR << "TcpServer accept error callback threw a non-standard exception";
                action = AcceptorErrorAction::Stop;
            }
        }
        if (action == AcceptorErrorAction::Stop) {
            stopInLoop();
        }
        return;
    }
    const InetAddress localAddr(localStorage);
    auto connection = std::make_shared<TcpConnection>(
        ioLoop,
        connName,
        pendingSocket.fd(),
        localAddr,
        peerAddr);
    (void)pendingSocket.releaseFd();
    connections_[connName] = connection;
    trackAcceptedConnection(connection, peerAddress);

    std::weak_ptr<void> lifetime = lifetimeToken_;
    CloseCallback closeCallback = [this, lifetime](const TcpConnectionPtr& conn) {
        if (lifetime.lock()) {
            removeConnection(conn);
        }
    };

    EventLoop::Functor establish =
        [connection,
         connectionCallback = connectionCallback_,
         messageCallback = messageCallback_,
         highWaterMarkCallback = highWaterMarkCallback_,
         writeCompleteCallback = writeCompleteCallback_,
         closeInfoCallback = closeInfoCallback_,
         callbackExceptionHandler = callbackExceptionHandler_,
         closeCallback = std::move(closeCallback),
         backpressureOptions = backpressureOptions_,
         highWaterMark = highWaterMark_]() mutable {
            connection->setBackpressureOptions(backpressureOptions);
            connection->setConnectionCallback(std::move(connectionCallback));
            connection->setMessageCallback(std::move(messageCallback));
            if (highWaterMarkCallback && highWaterMark > 0) {
                connection->setHighWaterMarkCallback(std::move(highWaterMarkCallback), highWaterMark);
            }
            connection->setWriteCompleteCallback(std::move(writeCompleteCallback));
            connection->setCloseInfoCallback(std::move(closeInfoCallback));
            connection->setCallbackExceptionHandler(std::move(callbackExceptionHandler));
            connection->setCloseCallback(std::move(closeCallback));
            connection->connectEstablished();
        };
    try {
        ioLoop->runInLoop(std::move(establish));
    } catch (const std::exception& error) {
        LOG_ERROR << "TcpServer failed to schedule connection establishment: "
                  << error.what();
        if (ioLoop == loop_ && !connection->disconnected()) {
            connection->connectDestroyed();
        }
        connections_.erase(connName);
        releaseConnectionAdmission(connName);
    } catch (...) {
        LOG_ERROR << "TcpServer failed to schedule connection establishment with a non-standard exception";
        if (ioLoop == loop_ && !connection->disconnected()) {
            connection->connectDestroyed();
        }
        connections_.erase(connName);
        releaseConnectionAdmission(connName);
    }
}

void TcpServer::removeConnection(const TcpConnectionPtr& connection) {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime, connection] {
        if (lifetime.lock()) {
            removeConnectionInLoop(connection);
        }
    });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    if (aggregateStopState_ && !aggregateStopState_->joined) {
        // The active aggregate owns worker cleanup and base-map release for
        // this generation. A normal close notification already committed
        // before stop is deliberately coalesced into that handshake.
        return;
    }
    const auto erased = connections_.erase(connection->name());
    if (erased == 0) {
        return;
    }
    releaseConnectionAdmission(connection->name());

    EventLoop* connectionLoop = connection->getLoop();
    connectionLoop->queueInLoop([connection] { connection->connectDestroyed(); });

    const auto state = gracefulStopStateSnapshot();
    if (state && !state->isCompleted() && !state->forceStarted && connections_.empty()) {
        std::weak_ptr<void> lifetime = lifetimeToken_;
        loop_->queueInLoop([this, lifetime, state] {
            if (!lifetime.lock()) {
                state->complete(TcpServerStopResult{
                    .outcome = TcpServerStopOutcome::ServerDestroyed,
                    .initialConnectionCount = state->initialConnectionCount,
                    .forcedConnectionCount = state->forcedConnectionCount,
                });
                return;
            }
            if (!state->isCompleted() && !state->forceStarted && connections_.empty()) {
                finishGracefulStopInLoop(state, TcpServerStopOutcome::Drained);
            }
        });
    }
}

bool TcpServer::forceCloseAllConnections() {
    clearConnectionAdmission();
    auto connections = std::move(connections_);
    connections_.clear();
    if (connections.empty()) {
        return false;
    }

    auto remaining = std::make_shared<std::atomic<std::size_t>>(connections.size());
    std::weak_ptr<void> lifetime = lifetimeToken_;
    EventLoop* baseLoop = loop_;
    EventLoopThreadPool* threadPool = threadPool_.get();
    auto notifyClosed = [remaining, lifetime, baseLoop, threadPool] {
        if (remaining->fetch_sub(1) != 1) {
            return;
        }

        baseLoop->runInLoop([lifetime, threadPool] {
            if (lifetime.lock()) {
                threadPool->stop();
            }
        });
    };

    for (auto& [name, connection] : connections) {
        (void)name;
        EventLoop* connectionLoop = connection->getLoop();
        connectionLoop->runInLoop([connection, notifyClosed] {
            connection->setCloseCallback([notifyClosed](const TcpConnectionPtr& conn) {
                conn->connectDestroyed();
                notifyClosed();
            });
            if (!connection->disconnected()) {
                connection->forceClose();
                return;
            }
            connection->connectDestroyed();
            notifyClosed();
        });
    }

    return true;
}

bool TcpServer::forceCloseAllConnectionsForGraceful(
    const std::shared_ptr<GracefulStopState>& state,
    TcpServerStopOutcome outcome) {
    clearConnectionAdmission();
    auto connections = std::move(connections_);
    connections_.clear();
    if (connections.empty()) {
        return false;
    }

    auto remaining = std::make_shared<std::atomic<std::size_t>>(connections.size());
    std::weak_ptr<void> lifetime = lifetimeToken_;
    EventLoop* baseLoop = loop_;
    EventLoopThreadPool* threadPool = threadPool_.get();
    auto notifyClosed = [remaining, lifetime, baseLoop, threadPool, state, outcome] {
        if (remaining->fetch_sub(1) != 1) {
            return;
        }

        baseLoop->runInLoop([lifetime, threadPool, state, outcome] {
            if (!lifetime.lock()) {
                state->complete(TcpServerStopResult{
                    .outcome = TcpServerStopOutcome::ServerDestroyed,
                    .initialConnectionCount = state->initialConnectionCount,
                    .forcedConnectionCount = state->forcedConnectionCount,
                });
                return;
            }
            threadPool->stop();
            state->complete(TcpServerStopResult{
                .outcome = outcome,
                .initialConnectionCount = state->initialConnectionCount,
                .forcedConnectionCount = state->forcedConnectionCount,
            });
        });
    };

    for (auto& [name, connection] : connections) {
        (void)name;
        EventLoop* connectionLoop = connection->getLoop();
        connectionLoop->runInLoop([connection, notifyClosed] {
            connection->setCloseCallback([notifyClosed](const TcpConnectionPtr& conn) {
                conn->connectDestroyed();
                notifyClosed();
            });
            if (!connection->disconnected()) {
                connection->forceClose();
                return;
            }
            connection->connectDestroyed();
            notifyClosed();
        });
    }

    return true;
}

}  // namespace gamenet::net
