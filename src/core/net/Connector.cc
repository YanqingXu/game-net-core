#include "gamenet/core/net/Connector.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
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
    std::uint64_t generation{0};
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
      initialRetryDelay_(options.initRetryDelay),
      maxRetryDelayMs_(options.maxRetryDelay),
      connectTimeout_(options.connectTimeout) {
    options.validate();
}

Connector::~Connector() {
    loop_->assertInLoopThread();
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
    loop_->assertInLoopThread();
    newConnectionCallback_ = std::move(cb);
}

void Connector::setConnectorEventCallback(ConnectorEventCallback cb) {
    loop_->assertInLoopThread();
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
    return state_.load(std::memory_order_acquire);
}

void Connector::start() {
    loop_->assertInLoopThread();
    connect_ = true;

    if (state() == kConnected) {
        return;
    }

    const std::uint64_t generation = ++requestGeneration_;
    if (state() == kConnecting) {
        if (connectTimeoutTimerId_.valid()) {
            loop_->cancel(connectTimeoutTimerId_);
            connectTimeoutTimerId_ = {};
        }
#ifdef _WIN32
        if (channel_ && cancelPendingConnectInLoop(channel_->fd())) {
            // finishCancelInLoop() observes the newer request generation and
            // starts it only after the old IOCP operation releases its slot.
            return;
        }
#endif
        if (channel_) {
            state_.store(kDisconnected, std::memory_order_release);
            const SocketFd staleFd = removeAndReleaseChannel();
            sockets::close(staleFd);
        }
    }
    startInLoop(generation);
}

void Connector::stop() {
    loop_->assertInLoopThread();
    connect_ = false;
    ++requestGeneration_;
    stopInLoop();
}

void Connector::restart() {
    loop_->assertInLoopThread();
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
    retryDelayMs_ = initialRetryDelay_;
    if (state() == kConnected) {
        state_.store(kDisconnected, std::memory_order_release);
    }
    start();
}

void Connector::setRetryDelay(Duration initial, Duration max) {
    loop_->assertInLoopThread();
    ConnectorOptions options;
    options.initRetryDelay = initial;
    options.maxRetryDelay = max;
    options.connectTimeout = connectTimeout_;
    options.enableRetry = retryEnabled();
    options.validate();

    initialRetryDelay_ = initial;
    retryDelayMs_ = initial;
    maxRetryDelayMs_ = max;
}

void Connector::setRetryEnabled(bool enabled) {
    loop_->assertInLoopThread();
    retryEnabled_.store(enabled, std::memory_order_release);
}

bool Connector::retryEnabled() const noexcept {
    return retryEnabled_.load(std::memory_order_acquire);
}

void Connector::startInLoop(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (!connect_ || generation != requestGeneration_) {
        return;
    }
    assert(state() == kDisconnected);
    connect(generation);
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
    if (state() == kConnecting) {
#ifdef _WIN32
        if (channel_ && cancelPendingConnectInLoop(channel_->fd())) {
            return;
        }
#endif
        state_.store(kDisconnected, std::memory_order_release);
        const SocketFd sockfd = removeAndReleaseChannel();
        sockets::close(sockfd);
        return;
    }
    if (state() == kConnected) {
        state_.store(kDisconnected, std::memory_order_release);
    }
}

void Connector::connect(std::uint64_t generation) {
    if (generation != requestGeneration_ || !connect_) {
        return;
    }
    emitEvent(ConnectorEvent::ConnectAttempt);

    if (generation != requestGeneration_ || !connect_) {
        return;
    }

    auto failBeforeChannel = [this, generation](int error) {
        LOG_ERROR << "Connector socket setup error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(kInvalidSocket, generation);
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
    connecting(sockfd, generation);

    iocpConnect_ = std::make_shared<IocpConnectState>();
    iocpConnect_->operation.channel = channel_.get();
    iocpConnect_->connectEx = connectEx;
    iocpConnect_->generation = generation;

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
        handleError(generation);
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
        connecting(sockfd, generation);
        return;
    }

    if (sockets::isConnectRetryable(savedError)) {
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd, generation);
        return;
    }

    LOG_ERROR << "Connector::connect error: " << sockets::errorMessage(savedError);
    emitEvent(ConnectorEvent::ConnectFailed);
    sockets::close(sockfd);
    if (generation == requestGeneration_ && connect_) {
        emitEvent(ConnectorEvent::TerminalFailure);
    }
#endif
}

void Connector::connecting(SocketFd sockfd, std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (generation != requestGeneration_ || !connect_) {
        sockets::close(sockfd);
        return;
    }
    state_.store(kConnecting, std::memory_order_release);
    assert(!channel_);
    channel_ = std::make_unique<Channel>(loop_, sockfd);
    auto weakThis = weak_from_this();
    channel_->setWriteCallback([weakThis, generation] {
        if (auto self = weakThis.lock()) {
            self->handleWrite(generation);
        }
    });
    channel_->setErrorCallback([weakThis, generation] {
        if (auto self = weakThis.lock()) {
            self->handleError(generation);
        }
    });
    channel_->enableWriting();

    // Register connect timeout timer if configured.
    if (connectTimeout_ > Duration::zero()) {
        connectTimeoutTimerId_ = loop_->runAfter(connectTimeout_, [self = shared_from_this(), generation] {
            self->handleConnectTimeout(generation);
        });
    }
}

void Connector::handleWrite(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (state() != kConnecting) {
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
            handleError(generation);
            return;
        }
        if (!platform::updateConnectContext(channel_->fd())) {
            const int error = sockets::lastError();
            if (connectTimeoutTimerId_.valid()) {
                loop_->cancel(connectTimeoutTimerId_);
                connectTimeoutTimerId_ = {};
            }
            const SocketFd sockfd = removeAndReleaseChannel();
            state_.store(kDisconnected, std::memory_order_release);
            LOG_ERROR << "Connector::handleWrite update connect context error: "
                      << error << " " << sockets::errorMessage(error);
            emitEvent(ConnectorEvent::ConnectFailed);
            retry(sockfd, generation);
            return;
        }
    }
#endif

    if (generation != requestGeneration_ || !connect_) {
#ifdef _WIN32
        if (iocpConnect_ && iocpConnect_->canceling) {
            finishCancelInLoop();
            return;
        }
#endif
        const SocketFd staleFd = removeAndReleaseChannel();
        sockets::close(staleFd);
        state_.store(kDisconnected, std::memory_order_release);
        return;
    }

    // Cancel connect timeout timer on success path.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    // Remove channel before delivering fd — ownership transfers to upper layer.
    const SocketFd sockfd = removeAndReleaseChannel();
    state_.store(kDisconnected, std::memory_order_release);

    const int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << sockets::errorMessage(err);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd, generation);
        return;
    }

    // Self-connect detection: compare local and peer addresses.
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(sockfd, &localStorage)) {
        const int error = sockets::lastError();
        LOG_ERROR << "Connector::handleWrite getsockname error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd, generation);
        return;
    }
    sockaddr_storage peerStorage{};
    if (!sockets::tryGetPeerAddr(sockfd, &peerStorage)) {
        const int error = sockets::lastError();
        LOG_ERROR << "Connector::handleWrite getpeername error: " << error << " "
                  << sockets::errorMessage(error);
        emitEvent(ConnectorEvent::ConnectFailed);
        retry(sockfd, generation);
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
        retry(sockfd, generation);
        return;
    }

    state_.store(kConnected, std::memory_order_release);
    emitEvent(ConnectorEvent::ConnectSuccess);
    auto settleCompletedAttempt = [this] {
        StateE completed = kConnected;
        (void)state_.compare_exchange_strong(
            completed,
            kDisconnected,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    };
    Socket connectedSocket(sockfd);
    if (generation == requestGeneration_ && connect_ && newConnectionCallback_) {
        const SocketFd transferredFd = connectedSocket.releaseFd();
        try {
            // Ownership linearizes at callback entry. The receiver must put
            // transferredFd under RAII before performing fallible work.
            newConnectionCallback_(transferredFd);
            if (generation != requestGeneration_ || !connect_) {
                // A callback may synchronously stop or restart Connector.
                // Settle only this attempt's still-current completed state;
                // never overwrite a newer generation's kConnecting state.
                settleCompletedAttempt();
            }
            return;
        } catch (const std::exception& error) {
            LOG_ERROR << "Connector new-connection callback threw: "
                      << error.what();
        } catch (...) {
            LOG_ERROR << "Connector new-connection callback threw "
                         "a non-standard exception";
        }
    }
    settleCompletedAttempt();
}

void Connector::handleError(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (state() != kConnecting) {
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
    state_.store(kDisconnected, std::memory_order_release);
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << sockets::errorMessage(err);
    emitEvent(ConnectorEvent::ConnectFailed);
    retry(sockfd, generation);
}

void Connector::handleConnectTimeout(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (generation != requestGeneration_) {
        return;
    }
    connectTimeoutTimerId_ = {};
    if (state() != kConnecting) {
        // Connection already completed (success or failure) before timeout.
        return;
    }

    LOG_WARN << "Connector::handleConnectTimeout: connect to "
             << serverAddr_.toIpPort() << " timed out";

    emitEvent(ConnectorEvent::ConnectTimeout);
    if (generation != requestGeneration_ || !connect_) {
        return;
    }

    // Close the connecting socket and retry or fail.
#ifdef _WIN32
    if (channel_ && iocpConnect_ && iocpConnect_->pending) {
        iocpConnect_->retryAfterCancel = connect_ && retryEnabled();
        if (cancelPendingConnectInLoop(channel_->fd())) {
            return;
        }
    }
#endif
    const SocketFd sockfd = removeAndReleaseChannel();
    sockets::close(sockfd);
    state_.store(kDisconnected, std::memory_order_release);

    retry(kInvalidSocket, generation);  // Socket was already closed.
}

void Connector::retry(SocketFd sockfd, std::uint64_t generation) {
    if (sockets::isValid(sockfd)) {
        sockets::close(sockfd);
    }
    if (generation != requestGeneration_) {
        return;
    }
    state_.store(kDisconnected, std::memory_order_release);
    if (connect_ && retryEnabled()) {
        emitEvent(ConnectorEvent::RetryScheduled);
        if (generation != requestGeneration_ || !connect_ || !retryEnabled()) {
            return;
        }
        // Schedule retry with backoff via EventLoop timer.
        retryTimerId_ = loop_->runAfter(retryDelayMs_, [self = shared_from_this(), generation] {
            if (generation != self->requestGeneration_ || !self->connect_) {
                return;
            }
            self->retryTimerId_ = {};
            self->startInLoop(generation);
        });
        // Exponential backoff: double the delay up to max.
        retryDelayMs_ = std::min(retryDelayMs_ * 2, maxRetryDelayMs_);
    } else if (connect_) {
        emitEvent(ConnectorEvent::TerminalFailure);
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
        iocpConnect_ && iocpConnect_->retryAfterCancel && connect_ &&
        retryEnabled() && iocpConnect_->generation == requestGeneration_;
    const bool startNewGeneration =
        iocpConnect_ && connect_ && iocpConnect_->generation != requestGeneration_;
    if (state() == kConnecting && channel_) {
        const SocketFd sockfd = removeAndReleaseChannel();
        sockets::close(sockfd);
    }

    state_.store(kDisconnected, std::memory_order_release);
    if (iocpConnect_) {
        iocpConnect_->pending = false;
        iocpConnect_->canceling = false;
        iocpConnect_->retryAfterCancel = false;
    }
    connectStopGuard_.reset();
    if (retryAfterCancel) {
        retry(kInvalidSocket, requestGeneration_);
    } else if (startNewGeneration) {
        startInLoop(requestGeneration_);
    } else if (connect_ && iocpConnect_ &&
               iocpConnect_->generation == requestGeneration_) {
        emitEvent(ConnectorEvent::TerminalFailure);
    }
}

#endif

SocketFd Connector::removeAndReleaseChannel() {
    auto removedChannel = std::move(channel_);
#ifdef _WIN32
    if (iocpConnect_ && !iocpConnect_->pending) {
        iocpConnect_->operation.channel = nullptr;
    }
#endif
    removedChannel->disableAll();
    removedChannel->remove();
    const SocketFd sockfd = removedChannel->fd();
    // Vacate the member slot before any re-entrant upper callback. If this is
    // the Channel currently being dispatched, EventLoop owns it only until
    // handleEvent() returns; otherwise retirement destroys it inline. This
    // path performs no pending-functor admission.
    loop_->retireCurrentChannel(std::move(removedChannel));
    return sockfd;
}

}  // namespace gamenet::net
