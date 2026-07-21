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
#include <exception>
#include <memory>
#include <utility>

namespace gamenet::net {

#ifdef _WIN32

struct Connector::IocpConnectState {
    IocpOperation operation{};
    LPFN_CONNECTEX connectEx{nullptr};
    bool pending{false};
    bool canceling{false};
    bool retryAfterCancel{false};

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

void Connector::emitEvent(ConnectorEvent event) noexcept {
    if (!connectorEventCallback_) {
        return;
    }
    try {
        connectorEventCallback_(serverAddr_, event);
    } catch (const std::exception& error) {
        LOG_ERROR << "Connector event callback threw: " << error.what();
    } catch (...) {
        LOG_ERROR << "Connector event callback threw a non-standard exception";
    }
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
        const SocketFd sockfd = removeAndReleaseChannel();
        sockets::close(sockfd);
    }
}

void Connector::connect() {
    emitEvent(ConnectorEvent::ConnectAttempt);

    auto failBeforeChannel = [this](int error) {
        LOG_ERROR << "Connector socket setup error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(kInvalidSocket);
    };

#ifdef _WIN32
    const SocketFd sockfd = platform::createOverlappedTcp(serverAddr_.family());
    if (!sockets::isValid(sockfd)) {
        failBeforeChannel(sockets::lastError());
        return;
    }
    if (!platform::bindUnspecified(sockfd, serverAddr_.family())) {
        const int error = sockets::lastError();
        sockets::close(sockfd);
        failBeforeChannel(error);
        return;
    }
    const auto connectEx = platform::loadConnectEx(sockfd);
    if (connectEx == nullptr) {
        const int error = sockets::lastError();
        sockets::close(sockfd);
        failBeforeChannel(error);
        return;
    }
    connecting(sockfd);

    iocpConnect_ = std::make_shared<IocpConnectState>();
    iocpConnect_->operation.channel = channel_.get();
    iocpConnect_->connectEx = connectEx;

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
    const int connectError = ok ? 0 : sockets::lastError();
    if (!ok && connectError != ERROR_IO_PENDING) {
        iocpConnect_->pending = false;
        iocpConnect_->operation.error = static_cast<DWORD>(connectError);
        handleError();
        return;
    }
    loop_->retainCompletionOperation(&iocpConnect_->operation, iocpConnect_);
    return;
#else
    const SocketFd sockfd = sockets::createNonblocking(serverAddr_.family());
    if (!sockets::isValid(sockfd)) {
        failBeforeChannel(sockets::lastError());
        return;
    }
    const int ret = sockets::connect(sockfd, serverAddr_.getSockAddr(), serverAddr_.getSockAddrLen());
    const int savedError = (ret == 0) ? 0 : sockets::lastError();

    if (savedError == 0 || sockets::isInProgress(savedError) || sockets::isInterrupted(savedError)) {
        connecting(sockfd);
        return;
    }

    if (sockets::isConnectRetryable(savedError)) {
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd);
        return;
    }

    LOG_ERROR << "Connector::connect error: " << sockets::errorMessage(savedError);
    emitEvent(ConnectorEvent::ConnectFailed);
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
        if (!platform::updateConnectContext(channel_->fd())) {
            const int error = sockets::lastError();
            if (connectTimeoutTimerId_.valid()) {
                loop_->cancel(connectTimeoutTimerId_);
                connectTimeoutTimerId_ = {};
            }
            const SocketFd sockfd = removeAndReleaseChannel();
            LOG_ERROR << "Connector::handleWrite update connect context error: "
                      << error << " " << sockets::errorMessage(error);
            emitEvent(ConnectorEvent::ConnectFailed);
            retry(sockfd);
            return;
        }
    }
#endif

    // Cancel connect timeout timer on success path.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    // Remove channel before delivering fd — ownership transfers to upper layer.
    const SocketFd sockfd = removeAndReleaseChannel();

    const int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << sockets::errorMessage(err);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd);
        return;
    }

    // Self-connect detection: compare local and peer addresses.
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(sockfd, &localStorage)) {
        const int error = sockets::lastError();
        LOG_ERROR << "Connector::handleWrite getsockname error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd);
        return;
    }
    sockaddr_storage peerStorage{};
    if (!sockets::tryGetPeerAddr(sockfd, &peerStorage)) {
        const int error = sockets::lastError();
        LOG_ERROR << "Connector::handleWrite getpeername error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd);
        return;
    }

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
        emitEvent(ConnectorEvent::SelfConnectDetected);
        retry(sockfd);
        return;
    }

    state_ = kConnected;
    emitEvent(ConnectorEvent::ConnectSuccess);
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

    const SocketFd sockfd = removeAndReleaseChannel();
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << sockets::errorMessage(err);
    emitEvent(ConnectorEvent::ConnectFailed);
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

    emitEvent(ConnectorEvent::ConnectTimeout);

    // Close the connecting socket and retry or fail.
#ifdef _WIN32
    if (channel_ && iocpConnect_ && iocpConnect_->pending) {
        iocpConnect_->retryAfterCancel = connect_ && retryEnabled_;
        if (cancelPendingConnectInLoop(channel_->fd())) {
            return;
        }
    }
#endif
    const SocketFd sockfd = removeAndReleaseChannel();
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
        emitEvent(ConnectorEvent::RetryScheduled);
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
        // The operation may already be complete with its IOCP packet still
        // queued. Keep the Channel/state alive until that packet is dequeued.
        return true;
    }

    return true;
}

void Connector::finishCancelInLoop() {
    loop_->assertInLoopThread();

    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    const bool retryAfterCancel =
        iocpConnect_ && iocpConnect_->retryAfterCancel && connect_ && retryEnabled_;
    if (state_ == kConnecting && channel_) {
        const SocketFd sockfd = removeAndReleaseChannel();
        sockets::close(sockfd);
    }

    state_ = kDisconnected;
    if (iocpConnect_) {
        iocpConnect_->pending = false;
        iocpConnect_->canceling = false;
        iocpConnect_->retryAfterCancel = false;
    }
    connectStopGuard_.reset();
    if (retryAfterCancel) {
        retry(kInvalidSocket);
    }
}

#endif

SocketFd Connector::removeAndReleaseChannel() {
#ifdef _WIN32
    if (iocpConnect_ && !iocpConnect_->pending) {
        iocpConnect_->operation.channel = nullptr;
    }
#endif
    channel_->disableAll();
    channel_->remove();
    const SocketFd sockfd = channel_->fd();
    auto deferredChannel = std::shared_ptr<Channel>(std::move(channel_));
    // The upper callback may tear down the new connection, exposing an already
    // queued restart before this dispatch unwinds. Vacate the member slot now,
    // but keep the removed Channel alive until event dispatch returns.
    loop_->queueInLoop([deferredChannel] { (void)deferredChannel; });
    return sockfd;
}

}  // namespace gamenet::net
