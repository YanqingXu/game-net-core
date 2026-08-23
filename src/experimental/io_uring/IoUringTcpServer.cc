// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/experimental/io_uring/IoUringTcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::experimental::io_uring {

namespace {

using namespace std::chrono_literals;

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

}  // namespace

class IoUringTcpServerImpl final {
public:
    IoUringTcpServerImpl(
        IoUringTcpServer* facade,
        gamenet::net::EventLoop* ownerLoop,
        gamenet::net::InetAddress listenAddress,
        IoUringTcpServerOptions options,
        IoUringTcpServer::StoppedConsumer stoppedConsumer)
        : facade_(facade),
          ownerLoop_(ownerLoop),
          requestedAddress_(std::move(listenAddress)),
          boundAddress_(requestedAddress_),
          options_(std::move(options)),
          stoppedConsumer_(std::move(stoppedConsumer)),
          stopFuture_(stopPromise_.get_future().share()) {
        if (facade_ == nullptr || ownerLoop_ == nullptr) {
            throw std::invalid_argument(
                "io_uring TCP Server requires facade and owner loop");
        }
        ownerLoop_->assertInLoopThread();
        options_.connection.validate();
        hub_ = std::make_unique<IoUringTcpConnectionHub>(
            ownerLoop_,
            options_.hub,
            [this](const IoUringTcpConnectionHubStopSummary& summary) {
                handleHubStopped(summary);
            });
        provisional_.reserve(options_.hub.maxPendingAccepts);
        connections_.reserve(options_.hub.maxConnections);
    }

    ~IoUringTcpServerImpl() {
        if (ownerLoop_ == nullptr || !ownerLoop_->isInLoopThread() ||
            phase_ != IoUringTcpServerPhase::Stopped ||
            stopFuture_.wait_for(0ms) != std::future_status::ready ||
            !provisional_.empty() || !connections_.empty()) {
            std::terminate();
        }
        hub_.reset();
    }

    template <typename Callback>
    void configure(Callback IoUringTcpServerImpl::*member, Callback callback) {
        assertOwner();
        if (phase_ != IoUringTcpServerPhase::Configuring) {
            throw std::logic_error(
                "io_uring TCP Server callbacks are sealed after start");
        }
        this->*member = std::move(callback);
    }

    void setConnectionCallback(IoUringTcpServer::ConnectionCallback callback) {
        configure(&IoUringTcpServerImpl::connectionCallback_, std::move(callback));
    }

    void setMessageCallback(IoUringTcpServer::MessageCallback callback) {
        configure(&IoUringTcpServerImpl::messageCallback_, std::move(callback));
    }

    void setHighWaterMarkCallback(
        IoUringTcpServer::HighWaterMarkCallback callback) {
        configure(&IoUringTcpServerImpl::highWaterCallback_, std::move(callback));
    }

    void setWriteCompleteCallback(
        IoUringTcpServer::WriteCompleteCallback callback) {
        configure(&IoUringTcpServerImpl::writeCompleteCallback_, std::move(callback));
    }

    void setCloseInfoCallback(IoUringTcpServer::CloseInfoCallback callback) {
        configure(&IoUringTcpServerImpl::closeInfoCallback_, std::move(callback));
    }

    IoUringTcpServerStartOutcome start() {
        assertOwner();
        ++metrics_.startAttempts;
        if (phase_ != IoUringTcpServerPhase::Configuring) {
            return {
                .result = IoUringTcpServerStartResult::AlreadyStarted,
                .listenAddress = boundAddress_,
            };
        }
        phase_ = IoUringTcpServerPhase::Starting;

        gamenet::net::Socket listener(
            gamenet::net::sockets::createNonblocking(
                requestedAddress_.family()));
        if (!gamenet::net::sockets::isValid(listener.fd())) {
            ++metrics_.socketCreateFailures;
            return failStart(
                IoUringTcpServerStartResult::SocketCreateFailed,
                gamenet::net::sockets::lastError());
        }
        listener.setReuseAddr(options_.reuseAddress);
        listener.setReusePort(options_.reusePort);
        if (requestedAddress_.isIpv6()) listener.setIpv6Only(false);

        int nativeError = 0;
        if (!listener.tryBindAddress(requestedAddress_, &nativeError)) {
            ++metrics_.bindFailures;
            return failStart(
                IoUringTcpServerStartResult::BindFailed,
                nativeError);
        }
        sockaddr_storage local{};
        if (!gamenet::net::sockets::tryGetLocalAddr(listener.fd(), &local)) {
            ++metrics_.listenFailures;
            return failStart(
                IoUringTcpServerStartResult::ListenFailed,
                gamenet::net::sockets::lastError());
        }
        boundAddress_ = gamenet::net::InetAddress(local);
        if (!listener.tryListen(&nativeError)) {
            ++metrics_.listenFailures;
            return failStart(
                IoUringTcpServerStartResult::ListenFailed,
                nativeError);
        }

        auto outcome = hub_->listen(
            listener.releaseFd(),
            [this] { return makeAcceptedConnectionCallbacks(); },
            [this](const IoUringTcpHubListenerStopSummary& summary) {
                handleListenerStopped(summary);
            });
        listenerFuture_ = outcome.stopFuture;
        const auto result = mapListenResult(outcome.result);
        if (result != IoUringTcpServerStartResult::Accepted) {
            ++metrics_.hubAdmissionFailures;
            if (phase_ != IoUringTcpServerPhase::Stopped) {
                phase_ = IoUringTcpServerPhase::Quiescing;
                maybeStopHub();
            }
            return {
                .result = result,
                .listenAddress = boundAddress_,
            };
        }
        phase_ = IoUringTcpServerPhase::Running;
        return {
            .result = IoUringTcpServerStartResult::Accepted,
            .listenAddress = boundAddress_,
        };
    }

    std::shared_future<IoUringTcpServerStopSummary> stopGracefully() {
        assertOwner();
        if (phase_ == IoUringTcpServerPhase::Stopped) return stopFuture_;
        if (phase_ != IoUringTcpServerPhase::Quiescing) {
            ++metrics_.gracefulStopRequests;
            beginStop(false);
        }
        return stopFuture_;
    }

    std::shared_future<IoUringTcpServerStopSummary> forceStop() {
        assertOwner();
        if (phase_ == IoUringTcpServerPhase::Stopped) return stopFuture_;
        ++metrics_.forceStopRequests;
        if (phase_ != IoUringTcpServerPhase::Quiescing) {
            beginStop(true);
        } else if (!forceRequested_) {
            forceRequested_ = true;
            metrics_.forceEscalated = true;
            forceConnections();
            maybeStopHub();
        }
        return stopFuture_;
    }

    IoUringTcpServerPhase phase() const {
        assertOwner();
        return phase_;
    }

    gamenet::net::InetAddress listenAddress() const {
        assertOwner();
        return boundAddress_;
    }

    std::size_t connectionCount() const {
        assertOwner();
        return connections_.size();
    }

    IoUringTcpServerMetrics metrics() const {
        assertOwner();
        auto snapshot = metrics_;
        snapshot.activeConnections = connections_.size();
        snapshot.provisionalConnections = provisional_.size();
        snapshot.listenerStopped = listenerStopped_;
        return snapshot;
    }

    std::shared_future<IoUringTcpServerStopSummary> stopFuture() const {
        return stopFuture_;
    }

private:
    using Adapter = IoUringTcpConnectionAdapter;

    void assertOwner() const {
        if (!ownerLoop_->isInLoopThread()) {
            throw std::runtime_error(
                "IoUringTcpServer used from a different thread");
        }
    }

    IoUringTcpServerStartOutcome failStart(
        IoUringTcpServerStartResult result,
        int nativeError) {
        phase_ = IoUringTcpServerPhase::Quiescing;
        listenerStopped_ = true;
        metrics_.listenerStopped = true;
        (void)hub_->stop();
        return {
            .result = result,
            .nativeError = nativeError,
            .listenAddress = boundAddress_,
        };
    }

    IoUringTcpHubAcceptedConnectionCallbacks
    makeAcceptedConnectionCallbacks() {
        assertOwner();
        auto connection = std::make_unique<Adapter>(
            ownerLoop_,
            hub_.get(),
            options_.connection);
        auto* observer = connection.get();
        connection->setMessageCallback(
            [this](Adapter& current, std::string_view payload) {
                if (!messageCallback_) return;
                try {
                    messageCallback_(current, payload);
                } catch (...) {
                    ++metrics_.callbackFailures;
                    throw;
                }
            });
        connection->setHighWaterMarkCallback(
            [this](Adapter& current, std::size_t bytes) {
                if (!highWaterCallback_) return;
                try {
                    highWaterCallback_(current, bytes);
                } catch (...) {
                    ++metrics_.callbackFailures;
                    throw;
                }
            });
        connection->setWriteCompleteCallback([this](Adapter& current) {
            if (!writeCompleteCallback_) return;
            try {
                writeCompleteCallback_(current);
            } catch (...) {
                ++metrics_.callbackFailures;
                throw;
            }
        });
        connection->setCloseInfoCallback(
            [this](Adapter& current,
                   const gamenet::net::TcpConnectionCloseInfo& closeInfo) {
                if (!closeInfoCallback_) return;
                try {
                    closeInfoCallback_(current, closeInfo);
                } catch (...) {
                    ++metrics_.callbackFailures;
                }
            });
        connection->setCloseCallback([this](Adapter& current) {
            handleConnectionClosed(current);
        });
        auto callbacks = connection->prepareAcceptedConnection(
            [this, observer](Adapter&, IoUringTcpHubAddResult result) {
                settleConnection(observer, result);
            });
        provisional_.push_back(std::move(connection));
        metrics_.provisionalConnections = provisional_.size();
        return callbacks;
    }

    void settleConnection(Adapter* observer, IoUringTcpHubAddResult result) {
        assertOwner();
        const auto found = std::find_if(
            provisional_.begin(),
            provisional_.end(),
            [observer](const auto& candidate) {
                return candidate.get() == observer;
            });
        if (found == provisional_.end()) {
            throw std::logic_error(
                "io_uring TCP Server lost provisional Adapter settlement");
        }
        if (result != IoUringTcpHubAddResult::Accepted) {
            ++metrics_.connectionAdmissionRejections;
            provisional_.erase(found);
            metrics_.provisionalConnections = provisional_.size();
            maybeStopHub();
            return;
        }

        connections_.push_back(std::move(*found));
        provisional_.erase(found);
        ++metrics_.connectionsEstablished;
        metrics_.activeConnections = connections_.size();
        metrics_.maxActiveConnections = (std::max)(
            metrics_.maxActiveConnections,
            connections_.size());
        metrics_.provisionalConnections = provisional_.size();

        if (phase_ != IoUringTcpServerPhase::Running &&
            phase_ != IoUringTcpServerPhase::Starting) {
            (void)observer->tryForceClose();
            return;
        }
        if (connectionCallback_) {
            try {
                connectionCallback_(*observer);
            } catch (...) {
                ++metrics_.callbackFailures;
                (void)observer->tryForceClose();
            }
        }
    }

    void handleConnectionClosed(Adapter& connection) noexcept {
        if (connectionCallback_) {
            try {
                connectionCallback_(connection);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
        const auto found = std::find_if(
            connections_.begin(),
            connections_.end(),
            [&connection](const auto& candidate) {
                return candidate.get() == &connection;
            });
        if (found == connections_.end()) {
            ++metrics_.callbackFailures;
            return;
        }
        connections_.erase(found);
        ++metrics_.connectionsRetired;
        metrics_.activeConnections = connections_.size();
        maybeStopHub();
    }

    void beginStop(bool force) {
        if (phase_ == IoUringTcpServerPhase::Stopped) return;
        phase_ = IoUringTcpServerPhase::Quiescing;
        forceRequested_ = force;
        if (listenerFuture_.valid()) {
            (void)hub_->stopListening();
        } else {
            listenerStopped_ = true;
            metrics_.listenerStopped = true;
        }
        if (force) {
            forceConnections();
        } else {
            for (const auto& connection : connections_) {
                (void)connection->tryShutdown();
            }
        }
        maybeStopHub();
    }

    void forceConnections() noexcept {
        for (const auto& connection : connections_) {
            try {
                (void)connection->tryForceClose();
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
    }

    void handleListenerStopped(
        const IoUringTcpHubListenerStopSummary&) noexcept {
        listenerStopped_ = true;
        metrics_.listenerStopped = true;
        maybeStopHub();
    }

    void maybeStopHub() noexcept {
        if (phase_ != IoUringTcpServerPhase::Quiescing ||
            !listenerStopped_ || !provisional_.empty() ||
            !connections_.empty() || hubStopRequested_) {
            return;
        }
        try {
            hubStopRequested_ = true;
            (void)hub_->stop();
        } catch (...) {
            hubStopRequested_ = false;
            ++metrics_.callbackFailures;
        }
    }

    void handleHubStopped(
        const IoUringTcpConnectionHubStopSummary& summary) noexcept {
        phase_ = IoUringTcpServerPhase::Stopped;
        metrics_.activeConnections = connections_.size();
        metrics_.provisionalConnections = provisional_.size();
        metrics_.listenerStopped = listenerStopped_;
        const auto stopped = IoUringTcpServerStopSummary{
            .server = metrics_,
            .hub = summary,
            .listenerStoppedBeforeHub = listenerStopped_,
            .allAdaptersRetired =
                connections_.empty() && provisional_.empty(),
        };
        if (!stopPublished_) {
            stopPublished_ = true;
            try {
                stopPromise_.set_value(stopped);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
        if (stoppedConsumer_) {
            try {
                stoppedConsumer_(stopped);
            } catch (...) {
                ++metrics_.callbackFailures;
            }
        }
    }

    IoUringTcpServer* facade_;
    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::InetAddress requestedAddress_;
    gamenet::net::InetAddress boundAddress_;
    IoUringTcpServerOptions options_;
    IoUringTcpServer::StoppedConsumer stoppedConsumer_;
    IoUringTcpServer::ConnectionCallback connectionCallback_;
    IoUringTcpServer::MessageCallback messageCallback_;
    IoUringTcpServer::HighWaterMarkCallback highWaterCallback_;
    IoUringTcpServer::WriteCompleteCallback writeCompleteCallback_;
    IoUringTcpServer::CloseInfoCallback closeInfoCallback_;
    std::promise<IoUringTcpServerStopSummary> stopPromise_;
    std::shared_future<IoUringTcpServerStopSummary> stopFuture_;
    std::unique_ptr<IoUringTcpConnectionHub> hub_;
    std::shared_future<IoUringTcpHubListenerStopSummary> listenerFuture_;
    std::vector<std::unique_ptr<Adapter>> provisional_;
    std::vector<std::unique_ptr<Adapter>> connections_;
    IoUringTcpServerMetrics metrics_{};
    IoUringTcpServerPhase phase_{IoUringTcpServerPhase::Configuring};
    bool listenerStopped_{false};
    bool forceRequested_{false};
    bool hubStopRequested_{false};
    bool stopPublished_{false};
};

IoUringTcpServer::IoUringTcpServer(
    gamenet::net::EventLoop* ownerLoop,
    gamenet::net::InetAddress listenAddress,
    IoUringTcpServerOptions options,
    StoppedConsumer stoppedConsumer)
    : impl_(std::make_unique<IoUringTcpServerImpl>(
          this,
          ownerLoop,
          std::move(listenAddress),
          std::move(options),
          std::move(stoppedConsumer))) {}

IoUringTcpServer::~IoUringTcpServer() = default;

void IoUringTcpServer::setConnectionCallback(ConnectionCallback callback) {
    impl_->setConnectionCallback(std::move(callback));
}

void IoUringTcpServer::setMessageCallback(MessageCallback callback) {
    impl_->setMessageCallback(std::move(callback));
}

void IoUringTcpServer::setHighWaterMarkCallback(
    HighWaterMarkCallback callback) {
    impl_->setHighWaterMarkCallback(std::move(callback));
}

void IoUringTcpServer::setWriteCompleteCallback(
    WriteCompleteCallback callback) {
    impl_->setWriteCompleteCallback(std::move(callback));
}

void IoUringTcpServer::setCloseInfoCallback(CloseInfoCallback callback) {
    impl_->setCloseInfoCallback(std::move(callback));
}

IoUringTcpServerStartOutcome IoUringTcpServer::start() {
    return impl_->start();
}

std::shared_future<IoUringTcpServerStopSummary>
IoUringTcpServer::stopGracefully() {
    return impl_->stopGracefully();
}

std::shared_future<IoUringTcpServerStopSummary>
IoUringTcpServer::forceStop() {
    return impl_->forceStop();
}

IoUringTcpServerPhase IoUringTcpServer::phase() const {
    return impl_->phase();
}

gamenet::net::InetAddress IoUringTcpServer::listenAddress() const {
    return impl_->listenAddress();
}

std::size_t IoUringTcpServer::connectionCount() const {
    return impl_->connectionCount();
}

IoUringTcpServerMetrics IoUringTcpServer::metrics() const {
    return impl_->metrics();
}

std::shared_future<IoUringTcpServerStopSummary>
IoUringTcpServer::stopFuture() const {
    return impl_->stopFuture();
}

}  // namespace gamenet::experimental::io_uring
