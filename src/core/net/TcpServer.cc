#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TimerId.h"

#include "gamenet/core/base/Logger.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

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
    bool forceStarted{false};
    std::size_t initialConnectionCount{0};
    std::size_t forcedConnectionCount{0};
    TimerId timeoutTimer;
};

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, reusePort)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_)) {
    acceptor_->setNewConnectionCallback(
        [this](SocketFd sockfd, const InetAddress& peerAddr) { newConnection(sockfd, peerAddr); });
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
    return connections_.size();
}

void TcpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    stopped_ = false;
    try {
        threadPool_->start(threadInitCallback_);
    } catch (...) {
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
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            stopInLoop();
        }
    });
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
        gracefulStopState_ = state;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    auto begin = [this, lifetime, state, options] {
        if (!lifetime.lock()) {
            state->complete(TcpServerStopResult{
                .outcome = TcpServerStopOutcome::ServerDestroyed,
            });
            return;
        }
        beginGracefulStopInLoop(state, options);
    };

    if (loop_->isInLoopThread()) {
        begin();
        return state->future;
    }

    try {
        loop_->queueInLoop(std::move(begin));
    } catch (...) {
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

void TcpServer::stopInLoop() {
    loop_->assertInLoopThread();
    if (stopped_) {
        const auto state = gracefulStopStateSnapshot();
        if (state && !state->isCompleted() && !state->forceStarted) {
            forceGracefulStopInLoop(state, TcpServerStopOutcome::ForcedByImmediateStop);
        }
        return;
    }

    stopped_ = true;
    acceptor_->setNewConnectionCallback({});
    if (acceptor_->listening()) {
        acceptor_->stop();
    }
    if (!forceCloseAllConnections()) {
        threadPool_->stop();
    }
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

    stopped_ = true;
    acceptor_->setNewConnectionCallback({});
    if (acceptor_->listening()) {
        acceptor_->stop();
    }

    state->initialConnectionCount = connections_.size();
    if (connections_.empty()) {
        finishGracefulStopInLoop(state, TcpServerStopOutcome::Drained);
        return;
    }

    for (const auto& [name, connection] : connections_) {
        (void)name;
        connection->shutdown();
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
    if (!forceCloseAllConnectionsForGraceful(state, outcome)) {
        finishGracefulStopInLoop(state, outcome);
    }
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
