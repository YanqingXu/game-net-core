#include "gamenet/core/net/Connector.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#include "gamenet/core/net/platform/IocpSocketOps.h"
#endif

#include <cassert>
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace gamenet::net {

#ifdef _WIN32

struct Connector::IocpConnectState {
    IocpOperation operation{};
    LPFN_CONNECTEX connectEx{nullptr};
    bool pending{false};
    bool canceling{false};

    IocpConnectState() {
        operation.kind = IocpOperationKind::Connect;
    }
};

#endif

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : Connector(loop, serverAddr, ConnectorOptions{}) {
}

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options)
    : loop_(loop),
      serverAddr_(serverAddr),
      state_(kDisconnected),
      connect_(false),
      retryEnabled_(options.enableRetry),
      retryDelayMs_(options.initRetryDelay),
      maxRetryDelayMs_(options.maxRetryDelay),
      connectTimeout_(options.connectTimeout) {
    options.validate();
}

Connector::~Connector() {
    // Cancel any pending retry timer.
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    // Cancel any pending connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
    // Channel should already be removed before destruction.
    assert(!channel_);
}

void Connector::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
}

void Connector::setConnectorEventCallback(ConnectorEventCallback cb) {
    connectorEventCallback_ = std::move(cb);
}

const InetAddress& Connector::serverAddress() const noexcept {
    return serverAddr_;
}

Connector::StateE Connector::state() const noexcept {
    return state_;
}

void Connector::start() {
    connect_ = true;
    loop_->runInLoop([self = shared_from_this()] { self->startInLoop(); });
}

void Connector::stop() {
    connect_ = false;
    loop_->runInLoop([self = shared_from_this()] { self->stopInLoop(); });
}

void Connector::restart() {
    loop_->assertInLoopThread();
    state_ = kDisconnected;
    retryDelayMs_ = kDefaultInitRetryDelay;
    connect_ = true;
    startInLoop();
}

void Connector::setRetryDelay(Duration initial, Duration max) {
    ConnectorOptions options;
    options.initRetryDelay = initial;
    options.maxRetryDelay = max;
    options.connectTimeout = connectTimeout_;
    options.enableRetry = retryEnabled_;
    options.validate();

    retryDelayMs_ = initial;
    maxRetryDelayMs_ = max;
}

void Connector::setRetryEnabled(bool enabled) noexcept {
    retryEnabled_ = enabled;
}

bool Connector::retryEnabled() const noexcept {
    return retryEnabled_;
}

void Connector::startInLoop() {
    loop_->assertInLoopThread();
    if (!connect_) {
        return;
    }
    assert(state_ == kDisconnected);
    connect();
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    // Cancel pending retry timer.
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    // Cancel pending connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
    if (state_ == kConnecting) {
#ifdef _WIN32
        if (channel_ && cancelPendingConnectInLoop(channel_->fd())) {
            return;
        }
#endif
        state_ = kDisconnected;
        const SocketFd sockfd = removeAndResetChannel();
        sockets::close(sockfd);
    }
}

void Connector::connect() {
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectAttempt);
    }

#ifdef _WIN32
    const SocketFd sockfd = platform::createOverlappedTcpOrDie(serverAddr_.family());
    platform::bindUnspecifiedOrDie(sockfd, serverAddr_.family());
    connecting(sockfd);

    iocpConnect_ = std::make_unique<IocpConnectState>();
    iocpConnect_->operation.channel = channel_.get();
    iocpConnect_->connectEx = platform::loadConnectEx(sockfd);

    DWORD bytes = 0;
    iocpConnect_->pending = true;
    const BOOL ok = iocpConnect_->connectEx(
        sockfd,
        serverAddr_.getSockAddr(),
        serverAddr_.getSockAddrLen(),
        nullptr,
        0,
        &bytes,
        &iocpConnect_->operation.overlapped);
    if (!ok && sockets::lastError() != ERROR_IO_PENDING) {
        iocpConnect_->pending = false;
        iocpConnect_->operation.error = static_cast<DWORD>(sockets::lastError());
        handleError();
    }
    return;
#else
    const SocketFd sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
    const int ret = sockets::connect(sockfd, serverAddr_.getSockAddr(), serverAddr_.getSockAddrLen());
    const int savedError = (ret == 0) ? 0 : sockets::lastError();

    if (savedError == 0 || sockets::isInProgress(savedError) || sockets::isInterrupted(savedError)) {
        connecting(sockfd);
        return;
    }

    if (sockets::isConnectRetryable(savedError)) {
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
        }
        retry(sockfd);
        return;
    }

    LOG_ERROR << "Connector::connect error: " << sockets::errorMessage(savedError);
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
    }
    sockets::close(sockfd);
#endif
}

void Connector::connecting(SocketFd sockfd) {
    state_ = kConnecting;
    assert(!channel_);
    channel_ = std::make_unique<Channel>(loop_, sockfd);
    auto weakThis = weak_from_this();
    channel_->setWriteCallback([weakThis] {
        if (auto self = weakThis.lock()) {
            self->handleWrite();
        }
    });
    channel_->setErrorCallback([weakThis] {
        if (auto self = weakThis.lock()) {
            self->handleError();
        }
    });
    channel_->enableWriting();

    // Register connect timeout timer if configured.
    if (connectTimeout_ > Duration::zero()) {
        connectTimeoutTimerId_ = loop_->runAfter(connectTimeout_, [self = shared_from_this()] {
            self->handleConnectTimeout();
        });
    }
}

void Connector::handleWrite() {
    if (state_ != kConnecting) {
        return;
    }

#ifdef _WIN32
    if (iocpConnect_) {
        iocpConnect_->pending = false;
        if (iocpConnect_->canceling || !connect_) {
            finishCancelInLoop();
            return;
        }
        if (iocpConnect_->operation.error != 0) {
            handleError();
            return;
        }
        platform::updateConnectContextOrDie(channel_->fd());
    }
#endif

    // Cancel connect timeout timer on success path.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    // Remove channel before delivering fd — ownership transfers to upper layer.
    const SocketFd sockfd = removeAndResetChannel();

    const int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << sockets::errorMessage(err);
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
        }
        retry(sockfd);
        return;
    }

    // Self-connect detection: compare local and peer addresses.
    const sockaddr_storage localStorage = sockets::getLocalAddr(sockfd);
    const sockaddr_storage peerStorage = sockets::getPeerAddr(sockfd);

    bool selfConnect = false;
    if (localStorage.ss_family == peerStorage.ss_family) {
        if (localStorage.ss_family == AF_INET6) {
            const auto& local6 = *reinterpret_cast<const sockaddr_in6*>(&localStorage);
            const auto& peer6 = *reinterpret_cast<const sockaddr_in6*>(&peerStorage);
            selfConnect = (local6.sin6_port == peer6.sin6_port) &&
                          (std::memcmp(&local6.sin6_addr, &peer6.sin6_addr, sizeof(in6_addr)) == 0);
        } else {
            const auto& local4 = *reinterpret_cast<const sockaddr_in*>(&localStorage);
            const auto& peer4 = *reinterpret_cast<const sockaddr_in*>(&peerStorage);
            selfConnect = (local4.sin_port == peer4.sin_port) &&
                          (local4.sin_addr.s_addr == peer4.sin_addr.s_addr);
        }
    }

    if (selfConnect) {
        LOG_WARN << "Connector::handleWrite self-connect detected, retrying";
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::SelfConnectDetected);
        }
        retry(sockfd);
        return;
    }

    state_ = kConnected;
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectSuccess);
    }
    if (connect_ && newConnectionCallback_) {
#ifdef _WIN32
        loop_->preserveSocketAssociation(sockfd);
#endif
        newConnectionCallback_(sockfd);
    } else {
        sockets::close(sockfd);
    }
}

void Connector::handleError() {
    if (state_ != kConnecting) {
        return;
    }

#ifdef _WIN32
    if (iocpConnect_) {
        iocpConnect_->pending = false;
        if (iocpConnect_->canceling || !connect_) {
            finishCancelInLoop();
            return;
        }
    }
#endif

    // Cancel connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    const SocketFd sockfd = removeAndResetChannel();
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << sockets::errorMessage(err);
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
    }
    retry(sockfd);
}

void Connector::handleConnectTimeout() {
    connectTimeoutTimerId_ = {};
    if (state_ != kConnecting) {
        // Connection already completed (success or failure) before timeout.
        return;
    }

    LOG_WARN << "Connector::handleConnectTimeout: connect to "
             << serverAddr_.toIpPort() << " timed out";

    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectTimeout);
    }

    // Close the connecting socket and retry or fail.
    const SocketFd sockfd = removeAndResetChannel();
    sockets::close(sockfd);
    state_ = kDisconnected;

    if (connect_ && retryEnabled_) {
        retry(kInvalidSocket);  // No socket to close (already closed), but schedule retry.
    }
}

void Connector::retry(SocketFd sockfd) {
    if (sockets::isValid(sockfd)) {
        sockets::close(sockfd);
    }
    state_ = kDisconnected;
    if (connect_ && retryEnabled_) {
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::RetryScheduled);
        }
        // Schedule retry with backoff via EventLoop timer.
        retryTimerId_ = loop_->runAfter(retryDelayMs_, [self = shared_from_this()] {
            self->retryTimerId_ = {};
            self->startInLoop();
        });
        // Exponential backoff: double the delay up to max.
        retryDelayMs_ = std::min(retryDelayMs_ * 2, maxRetryDelayMs_);
    }
}

#ifdef _WIN32

bool Connector::cancelPendingConnectInLoop(SocketFd sockfd) noexcept {
    if (!iocpConnect_ || !iocpConnect_->pending) {
        return false;
    }

    iocpConnect_->canceling = true;
    if (!connectStopGuard_) {
        connectStopGuard_ = shared_from_this();
    }

    if (::CancelIoEx(reinterpret_cast<HANDLE>(sockfd), &iocpConnect_->operation.overlapped) != FALSE) {
        return true;
    }

    const DWORD error = ::GetLastError();
    if (error == ERROR_NOT_FOUND || error == ERROR_INVALID_HANDLE) {
        iocpConnect_->pending = false;
        iocpConnect_->canceling = false;
        connectStopGuard_.reset();
        return false;
    }

    return true;
}

void Connector::finishCancelInLoop() {
    loop_->assertInLoopThread();

    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    if (state_ == kConnecting && channel_) {
        const SocketFd sockfd = removeAndResetChannel();
        sockets::close(sockfd);
    }

    state_ = kDisconnected;
    if (iocpConnect_) {
        iocpConnect_->pending = false;
        iocpConnect_->canceling = false;
    }
    connectStopGuard_.reset();
}

#endif

SocketFd Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    const SocketFd sockfd = channel_->fd();
    // Can't reset channel_ here because we're inside a channel callback.
    // Defer the reset via queueInLoop.
    loop_->queueInLoop([self = shared_from_this()] { self->resetChannel(); });
    return sockfd;
}

void Connector::resetChannel() {
    channel_.reset();
}

}  // namespace gamenet::net
