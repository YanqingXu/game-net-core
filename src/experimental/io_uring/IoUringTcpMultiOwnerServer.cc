// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "IoUringTcpMultiOwnerServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "core/net/detail/EventLoopLifecycleRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gamenet::experimental::io_uring {

namespace {

using namespace std::chrono_literals;

void validatePlacementPolicy(gamenet::net::EventLoopSelectionPolicy policy) {
    switch (policy) {
    case gamenet::net::EventLoopSelectionPolicy::RoundRobin:
    case gamenet::net::EventLoopSelectionPolicy::LeastConnections:
    case gamenet::net::EventLoopSelectionPolicy::QueueLag:
    case gamenet::net::EventLoopSelectionPolicy::ConsistentHash:
        return;
    }
    throw std::invalid_argument("unknown IOE-X12 placement policy");
}

IoUringTcpServerStartResult mapListenResult(
    IoUringTcpHubListenResult result) noexcept {
    switch (result) {
    case IoUringTcpHubListenResult::Accepted:
        return IoUringTcpServerStartResult::Accepted;
    case IoUringTcpHubListenResult::EngineRejected:
        return IoUringTcpServerStartResult::EngineRejected;
    case IoUringTcpHubListenResult::RejectedQuiescing:
        return IoUringTcpServerStartResult::RejectedQuiescing;
    case IoUringTcpHubListenResult::RejectedShutdown:
        return IoUringTcpServerStartResult::RejectedShutdown;
    case IoUringTcpHubListenResult::AlreadyListening:
    case IoUringTcpHubListenResult::RejectedInvalid:
        return IoUringTcpServerStartResult::HubRejected;
    }
    return IoUringTcpServerStartResult::HubRejected;
}

template <typename Value>
void updateAtomicMaximum(std::atomic<Value>& target, Value candidate) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < candidate &&
           !target.compare_exchange_weak(
               current,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

}  // namespace

class IoUringTcpMultiOwnerServerImpl final {
private:
    using Adapter = IoUringTcpConnectionAdapter;

    struct CallbackBundle {
        IoUringTcpMultiOwnerServer::ConnectionCallback connection;
        IoUringTcpMultiOwnerServer::MessageCallback message;
        IoUringTcpMultiOwnerServer::HighWaterMarkCallback highWater;
        IoUringTcpMultiOwnerServer::WriteCompleteCallback writeComplete;
        IoUringTcpMultiOwnerServer::CloseInfoCallback closeInfo;
    };

    struct HandoffEnvelope {
        explicit HandoffEnvelope(
            std::unique_ptr<gamenet::net::Socket> acceptedSocket)
            : socket(std::move(acceptedSocket)) {}

        std::unique_ptr<gamenet::net::Socket> socket;
    };

    class WorkerRuntime final {
    public:
        WorkerRuntime(
            IoUringTcpMultiOwnerServerImpl* server,
            std::size_t index,
            gamenet::net::EventLoop* loop,
            IoUringTcpConnectionHubOptions hubOptions,
            IoUringTcpConnectionAdapterOptions connectionOptions,
            CallbackBundle callbacks)
            : server_(server),
              index_(index),
              loop_(loop),
              executor_(loop == nullptr
                  ? gamenet::net::EventLoopExecutor{}
                  : loop->executor()),
              connectionOptions_(std::move(connectionOptions)),
              callbacks_(std::move(callbacks)) {
            if (server_ == nullptr || loop_ == nullptr) {
                throw std::invalid_argument(
                    "IOE-X12 worker requires Server and EventLoop");
            }
            loop_->assertInLoopThread();
            lifecycleSource_ = gamenet::net::detail::
                EventLoopLifecycleRegistry::attachQuiesceParticipant(
                    *loop_, [this] { driveLifecycle(); });
            lifecycleAttached_ = true;
            try {
                hub_ = std::make_unique<IoUringTcpConnectionHub>(
                    loop_,
                    std::move(hubOptions),
                    [this](const IoUringTcpConnectionHubStopSummary& summary) {
                        handleHubStopped(summary);
                    });
            } catch (...) {
                gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                    *loop_, lifecycleSource_);
                lifecycleAttached_ = false;
                cleaned_.store(true, std::memory_order_release);
                throw;
            }
        }

        ~WorkerRuntime() {
            if (!cleaned_.load(std::memory_order_acquire) || hub_ ||
                lifecycleAttached_ || !connections_.empty()) {
                std::terminate();
            }
        }

        gamenet::net::EventLoop* loop() const noexcept { return loop_; }
        gamenet::net::EventLoopExecutor executor() const noexcept {
            return executor_;
        }
        bool accepting() const noexcept {
            return stopMode_.load(std::memory_order_acquire) == 0 &&
                !cleaned_.load(std::memory_order_acquire) &&
                executor_.available();
        }
        bool cleaned() const noexcept {
            return cleaned_.load(std::memory_order_acquire);
        }

        bool admit(
            std::unique_ptr<gamenet::net::Socket> socket) noexcept {
            try {
                loop_->assertInLoopThread();
                if (!socket ||
                    !gamenet::net::sockets::isValid(socket->fd()) ||
                    stopMode_.load(std::memory_order_acquire) != 0 || !hub_ ||
                    hub_->phase() != IoUringTcpHubPhase::Running ||
                    !server_->admissionOpen_.load(std::memory_order_acquire)) {
                    ++admissionRejections_;
                    return false;
                }

                auto connection = std::make_unique<Adapter>(
                    loop_, hub_.get(), connectionOptions_);
                auto* observer = connection.get();
                connection->setMessageCallback(
                    [this](Adapter& current, std::string_view payload) {
                        if (!callbacks_.message) return;
                        try {
                            callbacks_.message(current, index_, payload);
                        } catch (...) {
                            ++callbackFailures_;
                            ++server_->callbackFailures_;
                            throw;
                        }
                    });
                connection->setHighWaterMarkCallback(
                    [this](Adapter& current, std::size_t bytes) {
                        if (!callbacks_.highWater) return;
                        try {
                            callbacks_.highWater(current, index_, bytes);
                        } catch (...) {
                            ++callbackFailures_;
                            ++server_->callbackFailures_;
                            throw;
                        }
                    });
                connection->setWriteCompleteCallback(
                    [this](Adapter& current) {
                        if (!callbacks_.writeComplete) return;
                        try {
                            callbacks_.writeComplete(current, index_);
                        } catch (...) {
                            ++callbackFailures_;
                            ++server_->callbackFailures_;
                            throw;
                        }
                    });
                connection->setCloseInfoCallback(
                    [this](
                        Adapter& current,
                        const gamenet::net::TcpConnectionCloseInfo& info) {
                        if (!callbacks_.closeInfo) return;
                        try {
                            callbacks_.closeInfo(current, index_, info);
                        } catch (...) {
                            ++callbackFailures_;
                            ++server_->callbackFailures_;
                        }
                    });
                connection->setCloseCallback(
                    [this](Adapter& current) { handleConnectionClosed(current); });

                const auto result = connection->establish(socket->releaseFd());
                if (result != IoUringTcpHubAddResult::Accepted) {
                    ++admissionRejections_;
                    return false;
                }
                connections_.push_back(std::move(connection));
                ++connectionsEstablished_;
                ++server_->connectionsEstablished_;
                const auto active = server_->activeConnections_.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                updateAtomicMaximum(server_->maxActiveConnections_, active);

                if (stopMode_.load(std::memory_order_acquire) != 0 ||
                    !server_->admissionOpen_.load(std::memory_order_acquire)) {
                    (void)observer->tryForceClose();
                    return true;
                }
                if (callbacks_.connection) {
                    try {
                        callbacks_.connection(*observer, index_);
                    } catch (...) {
                        ++callbackFailures_;
                        ++server_->callbackFailures_;
                        (void)observer->tryForceClose();
                    }
                }
                return true;
            } catch (...) {
                ++admissionRejections_;
                ++callbackFailures_;
                ++server_->callbackFailures_;
                return false;
            }
        }

        void requestStop(bool force) noexcept {
            const unsigned desired = force ? 2U : 1U;
            auto current = stopMode_.load(std::memory_order_acquire);
            while (current < desired &&
                   !stopMode_.compare_exchange_weak(
                       current,
                       desired,
                       std::memory_order_acq_rel,
                       std::memory_order_acquire)) {
            }
            (void)lifecycleSource_.signal();
        }

        IoUringTcpMultiOwnerWorkerStopSummary summary() const {
            if (!cleaned()) {
                throw std::logic_error(
                    "IOE-X12 worker summary requires physical cleanup");
            }
            return {
                .workerIndex = index_,
                .connectionsEstablished =
                    connectionsEstablished_.load(std::memory_order_acquire),
                .connectionsRetired =
                    connectionsRetired_.load(std::memory_order_acquire),
                .admissionRejections =
                    admissionRejections_.load(std::memory_order_acquire),
                .callbackFailures =
                    callbackFailures_.load(std::memory_order_acquire),
                .activeConnections = 0,
                .hub = hubSummary_.value(),
                .ownerDestroyedHub = true,
            };
        }

    private:
        void handleConnectionClosed(Adapter& connection) noexcept {
            if (callbacks_.connection) {
                try {
                    callbacks_.connection(connection, index_);
                } catch (...) {
                    ++callbackFailures_;
                    ++server_->callbackFailures_;
                }
            }
            const auto found = std::find_if(
                connections_.begin(),
                connections_.end(),
                [&connection](const auto& candidate) {
                    return candidate.get() == &connection;
                });
            if (found == connections_.end()) {
                ++callbackFailures_;
                ++server_->callbackFailures_;
                return;
            }
            connections_.erase(found);
            ++connectionsRetired_;
            ++server_->connectionsRetired_;
            server_->activeConnections_.fetch_sub(1, std::memory_order_acq_rel);
            server_->releasePlacement(index_);
            (void)lifecycleSource_.signal();
            (void)server_->baseLifecycleSource_.signal();
        }

        void handleHubStopped(
            const IoUringTcpConnectionHubStopSummary& summary) noexcept {
            hubSummary_ = summary;
            hubStopped_ = true;
            (void)lifecycleSource_.signal();
        }

        void driveLifecycle() noexcept {
            try {
                loop_->assertInLoopThread();
                if (cleaned_.load(std::memory_order_acquire)) return;
                if (loop_->phase() != gamenet::net::EventLoopPhase::Running) {
                    stopMode_.store(2, std::memory_order_release);
                }

                if (hubStopped_) {
                    if (!connections_.empty()) {
                        const auto stranded = connections_.size();
                        callbackFailures_.fetch_add(
                            stranded, std::memory_order_relaxed);
                        server_->callbackFailures_.fetch_add(
                            stranded, std::memory_order_relaxed);
                        connectionsRetired_.fetch_add(
                            stranded, std::memory_order_relaxed);
                        server_->connectionsRetired_.fetch_add(
                            stranded, std::memory_order_relaxed);
                        server_->activeConnections_.fetch_sub(
                            stranded, std::memory_order_acq_rel);
                        for (std::size_t index = 0; index < stranded; ++index) {
                            server_->releasePlacement(index_);
                        }
                        connections_.clear();
                    }
                    hub_.reset();
                    if (lifecycleAttached_) {
                        gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                            *loop_, lifecycleSource_);
                        lifecycleAttached_ = false;
                    }
                    cleaned_.store(true, std::memory_order_release);
                    (void)server_->baseLifecycleSource_.signal();
                    return;
                }

                const auto mode = stopMode_.load(std::memory_order_acquire);
                if (mode == 0 || !hub_) return;
                if (mode == 2 && !forceIssued_) {
                    forceIssued_ = true;
                    for (const auto& connection : connections_) {
                        (void)connection->tryForceClose();
                    }
                } else if (!gracefulIssued_) {
                    gracefulIssued_ = true;
                    for (const auto& connection : connections_) {
                        (void)connection->tryShutdown();
                    }
                }
                if (connections_.empty() && !hubStopRequested_) {
                    hubStopRequested_ = true;
                    (void)hub_->stop();
                }
                if (loop_->phase() !=
                        gamenet::net::EventLoopPhase::Running &&
                    !hubStopped_) {
                    // In final drain only this active node may rearm itself.
                    // Keep the bounded continuation alive until the Hub has
                    // published physical Pump/Engine stop.
                    (void)lifecycleSource_.signal();
                }
            } catch (...) {
                ++callbackFailures_;
                ++server_->callbackFailures_;
                stopMode_.store(2, std::memory_order_release);
                (void)lifecycleSource_.signal();
            }
        }

        IoUringTcpMultiOwnerServerImpl* server_;
        std::size_t index_;
        gamenet::net::EventLoop* loop_;
        gamenet::net::EventLoopExecutor executor_;
        IoUringTcpConnectionAdapterOptions connectionOptions_;
        CallbackBundle callbacks_;
        gamenet::net::EventLoopLifecycleSource lifecycleSource_;
        std::unique_ptr<IoUringTcpConnectionHub> hub_;
        std::vector<std::unique_ptr<Adapter>> connections_;
        std::optional<IoUringTcpConnectionHubStopSummary> hubSummary_;
        std::atomic<std::uint64_t> connectionsEstablished_{0};
        std::atomic<std::uint64_t> connectionsRetired_{0};
        std::atomic<std::uint64_t> admissionRejections_{0};
        std::atomic<std::uint64_t> callbackFailures_{0};
        std::atomic<unsigned> stopMode_{0};
        std::atomic<bool> cleaned_{false};
        bool lifecycleAttached_{false};
        bool gracefulIssued_{false};
        bool forceIssued_{false};
        bool hubStopRequested_{false};
        bool hubStopped_{false};
    };

public:
    IoUringTcpMultiOwnerServerImpl(
        gamenet::net::EventLoop* acceptLoop,
        gamenet::net::InetAddress listenAddress,
        IoUringTcpMultiOwnerServerOptions options,
        IoUringTcpMultiOwnerServer::StoppedConsumer stoppedConsumer)
        : acceptLoop_(acceptLoop),
          requestedAddress_(std::move(listenAddress)),
          boundAddress_(requestedAddress_),
          options_(std::move(options)),
          stoppedConsumer_(std::move(stoppedConsumer)),
          stopFuture_(stopPromise_.get_future().share()),
          placementLoads_(options_.workerCount) {
        if (acceptLoop_ == nullptr || options_.workerCount == 0 ||
            options_.workerCount >
                static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
            options_.maxPendingHandoffs == 0) {
            throw std::invalid_argument(
                "IOE-X12 Server requires an owner, workers, and finite handoffs");
        }
        acceptLoop_->assertInLoopThread();
        validatePlacementPolicy(options_.placementPolicy);
        options_.connection.validate();
        for (auto& load : placementLoads_) {
            load.store(0, std::memory_order_relaxed);
        }

        baseLifecycleSource_ = gamenet::net::detail::
            EventLoopLifecycleRegistry::attachQuiesceParticipant(
                *acceptLoop_, [this] { driveBaseLifecycle(); });
        baseLifecycleAttached_ = true;
        try {
            acceptHub_ = std::make_unique<IoUringTcpConnectionHub>(
                acceptLoop_,
                options_.acceptHub,
                [this](const IoUringTcpConnectionHubStopSummary& summary) {
                    handleAcceptHubStopped(summary);
                });
            pool_ = std::make_unique<gamenet::net::EventLoopThreadPool>(
                acceptLoop_, "io-uring-worker-");
            pool_->setThreadNum(static_cast<int>(options_.workerCount));
            pool_->setLoopSelectionPolicy(options_.placementPolicy);
        } catch (...) {
            if (baseLifecycleAttached_) {
                gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                    *acceptLoop_, baseLifecycleSource_);
                baseLifecycleAttached_ = false;
            }
            throw;
        }
    }

    ~IoUringTcpMultiOwnerServerImpl() {
        if (acceptLoop_ == nullptr || !acceptLoop_->isInLoopThread() ||
            phase_ != IoUringTcpMultiOwnerServerPhase::Stopped ||
            stopFuture_.wait_for(0ms) != std::future_status::ready ||
            acceptHub_ || baseLifecycleAttached_ || !workers_.empty()) {
            std::terminate();
        }
        pool_.reset();
    }

    template <typename Callback>
    void configure(Callback CallbackBundle::*member, Callback callback) {
        assertOwner();
        if (phase_ != IoUringTcpMultiOwnerServerPhase::Configuring) {
            throw std::logic_error(
                "IOE-X12 Server callbacks are sealed after start");
        }
        callbacks_.*member = std::move(callback);
    }

    void setWorkerInitCallback(gamenet::net::ThreadInitCallback callback) {
        assertOwner();
        if (phase_ != IoUringTcpMultiOwnerServerPhase::Configuring) {
            throw std::logic_error(
                "IOE-X12 worker init callback is sealed after start");
        }
        workerInitCallback_ = std::move(callback);
    }

    void setConnectionCallback(
        IoUringTcpMultiOwnerServer::ConnectionCallback callback) {
        configure(&CallbackBundle::connection, std::move(callback));
    }
    void setMessageCallback(
        IoUringTcpMultiOwnerServer::MessageCallback callback) {
        configure(&CallbackBundle::message, std::move(callback));
    }
    void setHighWaterMarkCallback(
        IoUringTcpMultiOwnerServer::HighWaterMarkCallback callback) {
        configure(&CallbackBundle::highWater, std::move(callback));
    }
    void setWriteCompleteCallback(
        IoUringTcpMultiOwnerServer::WriteCompleteCallback callback) {
        configure(&CallbackBundle::writeComplete, std::move(callback));
    }
    void setCloseInfoCallback(
        IoUringTcpMultiOwnerServer::CloseInfoCallback callback) {
        configure(&CallbackBundle::closeInfo, std::move(callback));
    }

    IoUringTcpServerStartOutcome start() {
        assertOwner();
        ++baseMetrics_.startAttempts;
        if (phase_ != IoUringTcpMultiOwnerServerPhase::Configuring) {
            return {
                .result = IoUringTcpServerStartResult::AlreadyStarted,
                .listenAddress = boundAddress_,
            };
        }
        phase_ = IoUringTcpMultiOwnerServerPhase::Starting;

        try {
            std::size_t nextWorker = 0;
            pool_->start([this, &nextWorker](gamenet::net::EventLoop* loop) {
                if (workerInitCallback_) workerInitCallback_(loop);
                if (loop->phase() != gamenet::net::EventLoopPhase::Running) {
                    throw std::logic_error(
                        "IOE-X12 worker init closed its EventLoop");
                }
                const auto index = nextWorker++;
                auto runtime = std::make_shared<WorkerRuntime>(
                    this,
                    index,
                    loop,
                    options_.workerHub,
                    options_.connection,
                    callbacks_);
                std::lock_guard lock(startupMutex_);
                workers_.push_back(std::move(runtime));
            });
            poolStarted_ = true;
            if (workers_.size() != options_.workerCount) {
                ++baseMetrics_.hubAdmissionFailures;
                return failStart(
                    IoUringTcpServerStartResult::HubRejected, 0);
            }
        } catch (...) {
            ++baseMetrics_.hubAdmissionFailures;
            poolStarted_ = false;
            workers_.clear();
            return failStart(IoUringTcpServerStartResult::HubRejected, 0);
        }

        gamenet::net::Socket listener(
            gamenet::net::sockets::createNonblocking(
                requestedAddress_.family()));
        if (!gamenet::net::sockets::isValid(listener.fd())) {
            ++baseMetrics_.socketCreateFailures;
            return failStart(
                IoUringTcpServerStartResult::SocketCreateFailed,
                gamenet::net::sockets::lastError());
        }
        listener.setReuseAddr(options_.reuseAddress);
        listener.setReusePort(options_.reusePort);
        if (requestedAddress_.isIpv6()) listener.setIpv6Only(false);

        int nativeError = 0;
        if (!listener.tryBindAddress(requestedAddress_, &nativeError)) {
            ++baseMetrics_.bindFailures;
            return failStart(
                IoUringTcpServerStartResult::BindFailed, nativeError);
        }
        sockaddr_storage local{};
        if (!gamenet::net::sockets::tryGetLocalAddr(listener.fd(), &local)) {
            ++baseMetrics_.listenFailures;
            return failStart(
                IoUringTcpServerStartResult::ListenFailed,
                gamenet::net::sockets::lastError());
        }
        boundAddress_ = gamenet::net::InetAddress(local);
        if (!listener.tryListen(&nativeError)) {
            ++baseMetrics_.listenFailures;
            return failStart(
                IoUringTcpServerStartResult::ListenFailed, nativeError);
        }

        const auto outcome = acceptHub_->listenAndHandoff(
            listener.releaseFd(),
            [this](
                std::unique_ptr<gamenet::net::Socket> socket,
                const gamenet::net::InetAddress& peer) {
                return handoffAcceptedSocket(std::move(socket), peer);
            },
            [this](const IoUringTcpHubListenerStopSummary&) {
                listenerStopped_ = true;
                baseMetrics_.listenerStopped = true;
                (void)baseLifecycleSource_.signal();
            });
        listenerFuture_ = outcome.stopFuture;
        const auto result = mapListenResult(outcome.result);
        if (result != IoUringTcpServerStartResult::Accepted) {
            ++baseMetrics_.hubAdmissionFailures;
            return failStart(result, 0);
        }
        phase_ = IoUringTcpMultiOwnerServerPhase::Running;
        admissionOpen_.store(true, std::memory_order_release);
        return {
            .result = IoUringTcpServerStartResult::Accepted,
            .listenAddress = boundAddress_,
        };
    }

    std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
    stopGracefully() {
        assertOwner();
        if (phase_ == IoUringTcpMultiOwnerServerPhase::Stopped) {
            return stopFuture_;
        }
        if (phase_ != IoUringTcpMultiOwnerServerPhase::Quiescing) {
            ++baseMetrics_.gracefulStopRequests;
            beginStop(false);
        }
        return stopFuture_;
    }

    std::shared_future<IoUringTcpMultiOwnerServerStopSummary> forceStop() {
        assertOwner();
        if (phase_ == IoUringTcpMultiOwnerServerPhase::Stopped) {
            return stopFuture_;
        }
        ++baseMetrics_.forceStopRequests;
        if (phase_ != IoUringTcpMultiOwnerServerPhase::Quiescing) {
            beginStop(true);
        } else if (!forceRequested_) {
            forceRequested_ = true;
            baseMetrics_.forceEscalated = true;
            if (workersStopRequested_) {
                for (const auto& worker : workers_) worker->requestStop(true);
            }
            (void)baseLifecycleSource_.signal();
        }
        return stopFuture_;
    }

    IoUringTcpMultiOwnerServerPhase phase() const {
        assertOwner();
        return phase_;
    }

    gamenet::net::InetAddress listenAddress() const {
        assertOwner();
        return boundAddress_;
    }

    IoUringTcpMultiOwnerServerMetrics metrics() const {
        assertOwner();
        auto snapshot = baseMetrics_;
        snapshot.handoffsAccepted =
            handoffsAccepted_.load(std::memory_order_acquire);
        snapshot.workerAdmissionRejections =
            workerAdmissionRejections_.load(std::memory_order_acquire);
        snapshot.connectionsEstablished =
            connectionsEstablished_.load(std::memory_order_acquire);
        snapshot.connectionsRetired =
            connectionsRetired_.load(std::memory_order_acquire);
        snapshot.callbackFailures =
            callbackFailures_.load(std::memory_order_acquire);
        snapshot.pendingHandoffs =
            pendingHandoffs_.load(std::memory_order_acquire);
        snapshot.maxPendingHandoffs =
            maxPendingHandoffs_.load(std::memory_order_acquire);
        snapshot.activeConnections =
            activeConnections_.load(std::memory_order_acquire);
        snapshot.maxActiveConnections =
            maxActiveConnections_.load(std::memory_order_acquire);
        snapshot.listenerStopped = listenerStopped_;
        return snapshot;
    }

    std::vector<gamenet::net::EventLoopExecutor> workerExecutors() const {
        assertOwner();
        std::vector<gamenet::net::EventLoopExecutor> result;
        result.reserve(workers_.size());
        for (const auto& worker : workers_) {
            result.push_back(worker->executor());
        }
        return result;
    }

    std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
    stopFuture() const {
        return stopFuture_;
    }

private:
    void assertOwner() const {
        if (!acceptLoop_->isInLoopThread()) {
            throw std::runtime_error(
                "IOE-X12 Server used from a different thread");
        }
    }

    IoUringTcpServerStartOutcome failStart(
        IoUringTcpServerStartResult result,
        int nativeError) {
        beginStop(true);
        return {
            .result = result,
            .nativeError = nativeError,
            .listenAddress = boundAddress_,
        };
    }

    void beginStop(bool force) noexcept {
        if (phase_ == IoUringTcpMultiOwnerServerPhase::Stopped) return;
        phase_ = IoUringTcpMultiOwnerServerPhase::Quiescing;
        admissionOpen_.store(false, std::memory_order_release);
        forceRequested_ = force;
        if (listenerFuture_.valid()) {
            try {
                (void)acceptHub_->stopListening();
            } catch (...) {
                ++callbackFailures_;
            }
        } else {
            listenerStopped_ = true;
            baseMetrics_.listenerStopped = true;
        }
        (void)baseLifecycleSource_.signal();
    }

    std::size_t selectWorker(const gamenet::net::InetAddress& peer) {
        if (options_.placementPolicy ==
            gamenet::net::EventLoopSelectionPolicy::LeastConnections) {
            const auto count = workers_.size();
            const auto start = leastNext_ % count;
            auto best = start;
            for (std::size_t offset = 1; offset < count; ++offset) {
                const auto candidate = (start + offset) % count;
                if (placementLoads_[candidate].load(
                        std::memory_order_acquire) <
                    placementLoads_[best].load(std::memory_order_acquire)) {
                    best = candidate;
                }
            }
            leastNext_ = (best + 1) % count;
            return best;
        }

        const auto* selected = pool_->selectLoop(
            options_.placementPolicy ==
                    gamenet::net::EventLoopSelectionPolicy::ConsistentHash
                ? peer.toIp()
                : std::string{});
        const auto found = std::find_if(
            workers_.begin(),
            workers_.end(),
            [selected](const auto& worker) {
                return worker->loop() == selected;
            });
        if (found == workers_.end()) {
            throw std::logic_error(
                "IOE-X12 selector returned a non-worker EventLoop");
        }
        return static_cast<std::size_t>(
            std::distance(workers_.begin(), found));
    }

    IoUringTcpHubAcceptedSocketResult handoffAcceptedSocket(
        std::unique_ptr<gamenet::net::Socket> socket,
        const gamenet::net::InetAddress& peer) noexcept {
        try {
            assertOwner();
            if (!socket ||
                !gamenet::net::sockets::isValid(socket->fd())) {
                return IoUringTcpHubAcceptedSocketResult::RejectedInvalid;
            }
            if (phase_ != IoUringTcpMultiOwnerServerPhase::Running ||
                !admissionOpen_.load(std::memory_order_acquire)) {
                ++baseMetrics_.handoffShutdownRejections;
                return IoUringTcpHubAcceptedSocketResult::WorkerShutdown;
            }
            const auto pending = pendingHandoffs_.load(
                std::memory_order_acquire);
            if (pending >= options_.maxPendingHandoffs) {
                ++baseMetrics_.handoffLimitRejections;
                return IoUringTcpHubAcceptedSocketResult::QueueFull;
            }

            const auto workerIndex = selectWorker(peer);
            const auto& worker = workers_.at(workerIndex);
            if (!worker->accepting()) {
                ++baseMetrics_.handoffShutdownRejections;
                return IoUringTcpHubAcceptedSocketResult::WorkerShutdown;
            }

            placementLoads_[workerIndex].fetch_add(
                1, std::memory_order_acq_rel);
            const auto handoffs = pendingHandoffs_.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            updateAtomicMaximum(maxPendingHandoffs_, handoffs);
            auto envelope = std::make_shared<HandoffEnvelope>(
                std::move(socket));
            const auto result = worker->executor().post(
                [this, worker, workerIndex, envelope] {
                    const bool admitted = worker->admit(
                        std::move(envelope->socket));
                    settleHandoff(workerIndex, admitted);
                });
            if (result == gamenet::net::PostResult::Accepted) {
                ++handoffsAccepted_;
                return IoUringTcpHubAcceptedSocketResult::Accepted;
            }

            pendingHandoffs_.fetch_sub(1, std::memory_order_acq_rel);
            releasePlacement(workerIndex);
            if (result == gamenet::net::PostResult::QueueFull) {
                ++baseMetrics_.handoffQueueRejections;
                return IoUringTcpHubAcceptedSocketResult::QueueFull;
            }
            ++baseMetrics_.handoffShutdownRejections;
            return IoUringTcpHubAcceptedSocketResult::WorkerShutdown;
        } catch (...) {
            ++callbackFailures_;
            return IoUringTcpHubAcceptedSocketResult::RejectedInvalid;
        }
    }

    void settleHandoff(std::size_t workerIndex, bool admitted) noexcept {
        pendingHandoffs_.fetch_sub(1, std::memory_order_acq_rel);
        if (!admitted) {
            releasePlacement(workerIndex);
            ++workerAdmissionRejections_;
        }
        (void)baseLifecycleSource_.signal();
    }

    void releasePlacement(std::size_t workerIndex) noexcept {
        const auto previous = placementLoads_[workerIndex].fetch_sub(
            1, std::memory_order_acq_rel);
        if (previous == 0) {
            placementLoads_[workerIndex].store(0, std::memory_order_release);
            ++callbackFailures_;
        }
    }

    void handleAcceptHubStopped(
        const IoUringTcpConnectionHubStopSummary& summary) noexcept {
        acceptHubSummary_ = summary;
        acceptHubStopped_ = true;
        (void)baseLifecycleSource_.signal();
    }

    bool allWorkersCleaned() const noexcept {
        return std::all_of(
            workers_.begin(), workers_.end(), [](const auto& worker) {
                return worker->cleaned();
            });
    }

    void driveBaseLifecycle() noexcept {
        try {
            assertOwner();
            if (acceptLoop_->phase() !=
                    gamenet::net::EventLoopPhase::Running &&
                phase_ != IoUringTcpMultiOwnerServerPhase::Stopped) {
                if (phase_ != IoUringTcpMultiOwnerServerPhase::Quiescing) {
                    ++baseMetrics_.forceStopRequests;
                    beginStop(true);
                } else if (!forceRequested_) {
                    forceRequested_ = true;
                    baseMetrics_.forceEscalated = true;
                }
            }
            if (phase_ != IoUringTcpMultiOwnerServerPhase::Quiescing) return;

            if (listenerStopped_ && !acceptHubStopRequested_ && acceptHub_) {
                acceptHubStopRequested_ = true;
                (void)acceptHub_->stop();
            }
            if (listenerStopped_ &&
                pendingHandoffs_.load(std::memory_order_acquire) == 0 &&
                !workersStopRequested_) {
                listenerStoppedBeforeWorkers_ = true;
                workersStopRequested_ = true;
                for (const auto& worker : workers_) {
                    worker->requestStop(forceRequested_);
                }
            } else if (workersStopRequested_ && forceRequested_) {
                for (const auto& worker : workers_) worker->requestStop(true);
            }

            if (acceptHubStopped_ && acceptHub_) {
                acceptHub_.reset();
            }
            if (!acceptHub_ && allWorkersCleaned()) {
                std::vector<IoUringTcpMultiOwnerWorkerStopSummary> summaries;
                summaries.reserve(workers_.size());
                for (const auto& worker : workers_) {
                    summaries.push_back(worker->summary());
                }
                if (poolStarted_) {
                    pool_->stop();
                    poolStarted_ = false;
                }
                workers_.clear();
                if (baseLifecycleAttached_) {
                    gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                        *acceptLoop_, baseLifecycleSource_);
                    baseLifecycleAttached_ = false;
                }
                phase_ = IoUringTcpMultiOwnerServerPhase::Stopped;
                baseMetrics_.listenerStopped = listenerStopped_;
                auto snapshot = metrics();
                const auto stopped = IoUringTcpMultiOwnerServerStopSummary{
                    .server = snapshot,
                    .acceptHub = acceptHubSummary_.value(),
                    .workers = std::move(summaries),
                    .listenerStoppedBeforeWorkers =
                        listenerStoppedBeforeWorkers_,
                    .allHandoffsSettled = snapshot.pendingHandoffs == 0,
                    .allWorkersStopped = true,
                };
                if (!stopPublished_) {
                    stopPublished_ = true;
                    stopPromise_.set_value(stopped);
                }
                if (stoppedConsumer_) {
                    try {
                        stoppedConsumer_(stopped);
                    } catch (...) {
                        ++callbackFailures_;
                    }
                }
            }
            if (phase_ != IoUringTcpMultiOwnerServerPhase::Stopped &&
                acceptLoop_->phase() !=
                    gamenet::net::EventLoopPhase::Running) {
                // Cross-node signals are sealed during final drain. The base
                // coordinator therefore retains its own bounded continuation
                // until listener, handoff, and worker cleanup converge.
                (void)baseLifecycleSource_.signal();
            }
        } catch (...) {
            ++callbackFailures_;
            if (baseLifecycleAttached_) {
                (void)baseLifecycleSource_.signal();
            }
        }
    }

    gamenet::net::EventLoop* acceptLoop_;
    gamenet::net::InetAddress requestedAddress_;
    gamenet::net::InetAddress boundAddress_;
    IoUringTcpMultiOwnerServerOptions options_;
    IoUringTcpMultiOwnerServer::StoppedConsumer stoppedConsumer_;
    gamenet::net::ThreadInitCallback workerInitCallback_;
    CallbackBundle callbacks_;
    std::promise<IoUringTcpMultiOwnerServerStopSummary> stopPromise_;
    std::shared_future<IoUringTcpMultiOwnerServerStopSummary> stopFuture_;
    gamenet::net::EventLoopLifecycleSource baseLifecycleSource_;
    std::unique_ptr<IoUringTcpConnectionHub> acceptHub_;
    std::unique_ptr<gamenet::net::EventLoopThreadPool> pool_;
    std::vector<std::shared_ptr<WorkerRuntime>> workers_;
    std::vector<std::atomic<std::size_t>> placementLoads_;
    std::shared_future<IoUringTcpHubListenerStopSummary> listenerFuture_;
    std::optional<IoUringTcpConnectionHubStopSummary> acceptHubSummary_;
    mutable std::mutex startupMutex_;
    IoUringTcpMultiOwnerServerMetrics baseMetrics_{};
    std::atomic<std::uint64_t> handoffsAccepted_{0};
    std::atomic<std::uint64_t> workerAdmissionRejections_{0};
    std::atomic<std::uint64_t> connectionsEstablished_{0};
    std::atomic<std::uint64_t> connectionsRetired_{0};
    std::atomic<std::uint64_t> callbackFailures_{0};
    std::atomic<std::size_t> pendingHandoffs_{0};
    std::atomic<std::size_t> maxPendingHandoffs_{0};
    std::atomic<std::size_t> activeConnections_{0};
    std::atomic<std::size_t> maxActiveConnections_{0};
    std::atomic<bool> admissionOpen_{false};
    IoUringTcpMultiOwnerServerPhase phase_{
        IoUringTcpMultiOwnerServerPhase::Configuring};
    std::size_t leastNext_{0};
    bool baseLifecycleAttached_{false};
    bool poolStarted_{false};
    bool listenerStopped_{false};
    bool listenerStoppedBeforeWorkers_{false};
    bool acceptHubStopRequested_{false};
    bool acceptHubStopped_{false};
    bool workersStopRequested_{false};
    bool forceRequested_{false};
    bool stopPublished_{false};

    friend class WorkerRuntime;
};

IoUringTcpMultiOwnerServer::IoUringTcpMultiOwnerServer(
    gamenet::net::EventLoop* acceptLoop,
    gamenet::net::InetAddress listenAddress,
    IoUringTcpMultiOwnerServerOptions options,
    StoppedConsumer stoppedConsumer)
    : impl_(std::make_unique<IoUringTcpMultiOwnerServerImpl>(
          acceptLoop,
          std::move(listenAddress),
          std::move(options),
          std::move(stoppedConsumer))) {}

IoUringTcpMultiOwnerServer::~IoUringTcpMultiOwnerServer() = default;

void IoUringTcpMultiOwnerServer::setWorkerInitCallback(
    gamenet::net::ThreadInitCallback callback) {
    impl_->setWorkerInitCallback(std::move(callback));
}

void IoUringTcpMultiOwnerServer::setConnectionCallback(
    ConnectionCallback callback) {
    impl_->setConnectionCallback(std::move(callback));
}

void IoUringTcpMultiOwnerServer::setMessageCallback(
    MessageCallback callback) {
    impl_->setMessageCallback(std::move(callback));
}

void IoUringTcpMultiOwnerServer::setHighWaterMarkCallback(
    HighWaterMarkCallback callback) {
    impl_->setHighWaterMarkCallback(std::move(callback));
}

void IoUringTcpMultiOwnerServer::setWriteCompleteCallback(
    WriteCompleteCallback callback) {
    impl_->setWriteCompleteCallback(std::move(callback));
}

void IoUringTcpMultiOwnerServer::setCloseInfoCallback(
    CloseInfoCallback callback) {
    impl_->setCloseInfoCallback(std::move(callback));
}

IoUringTcpServerStartOutcome IoUringTcpMultiOwnerServer::start() {
    return impl_->start();
}

std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
IoUringTcpMultiOwnerServer::stopGracefully() {
    return impl_->stopGracefully();
}

std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
IoUringTcpMultiOwnerServer::forceStop() {
    return impl_->forceStop();
}

IoUringTcpMultiOwnerServerPhase
IoUringTcpMultiOwnerServer::phase() const {
    return impl_->phase();
}

gamenet::net::InetAddress
IoUringTcpMultiOwnerServer::listenAddress() const {
    return impl_->listenAddress();
}

IoUringTcpMultiOwnerServerMetrics
IoUringTcpMultiOwnerServer::metrics() const {
    return impl_->metrics();
}

std::vector<gamenet::net::EventLoopExecutor>
IoUringTcpMultiOwnerServer::workerExecutors() const {
    return impl_->workerExecutors();
}

std::shared_future<IoUringTcpMultiOwnerServerStopSummary>
IoUringTcpMultiOwnerServer::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
