#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpOutputMemoryBudget.h"
#include "detail/ConnectionBackpressureController.h"
#include "detail/EventLoopLifecycleRegistry.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "platform/IocpTcpTransport.h"
#endif

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

namespace gamenet::net {

namespace {

constexpr std::uint64_t kCloseInfoPublished =
    std::uint64_t{1} << 63;
constexpr std::uint64_t kCloseReasonMask =
    std::uint64_t{0xFF} << 32;

std::uint64_t packCloseInfo(
    TcpConnectionCloseReason reason,
    int nativeError) noexcept {
    return kCloseInfoPublished |
        (static_cast<std::uint64_t>(reason) << 32) |
        static_cast<std::uint32_t>(nativeError);
}

TcpConnectionCloseInfo unpackCloseInfo(std::uint64_t bits) noexcept {
    return TcpConnectionCloseInfo{
        .reason = static_cast<TcpConnectionCloseReason>(
            (bits & kCloseReasonMask) >> 32),
        .nativeError = static_cast<int>(
            static_cast<std::int32_t>(bits & 0xFFFFFFFFu)),
    };
}

void removeChannelRegistrationInLoop(
    EventLoop* loop,
    Channel* channel,
    bool channelAdded,
    bool& channelRemoved) {
    loop->assertInLoopThread();
    if (!channelAdded || channelRemoved) {
        return;
    }
    if (!channel->isNoneEvent()) {
        channel->disableAll();
    }
    channel->remove();
    channelRemoved = true;
}

}  // namespace

void TcpConnectionBackpressureOptions::validate() const {
    detail::ConnectionBackpressureController::validateThresholds(
        highWaterMarkBytes,
        lowWaterMarkBytes);
    if (hardLimitBytes < highWaterMarkBytes) {
        throw std::invalid_argument(
            "backpressure hard limit must be at least the high water mark");
    }
    if (maxInputBufferBytes == 0) {
        throw std::invalid_argument("input buffer hard limit must be greater than zero");
    }
}

TcpConnection::TcpConnection(
    EventLoop* loop,
    std::string name,
    SocketFd sockfd,
    const InetAddress& localAddr,
    const InetAddress& peerAddr)
    : loop_(loop),
      name_(std::move(name)),
      state_(kConnecting),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      backpressure_(std::make_unique<detail::ConnectionBackpressureController>(loop)),
      localAddr_(localAddr),
      peerAddr_(peerAddr) {
    backpressureOptions_.validate();
#ifdef _WIN32
    iocpTransport_ = std::make_unique<IocpTcpTransport>(channel_.get());
#endif
    channel_->setReadCallback([this](gamenet::base::Timestamp receiveTime) { handleRead(receiveTime); });
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setCloseCallback([this] { handleClose(); });
    channel_->setErrorCallback([this] { handleError(); });
}

TcpConnection::~TcpConnection() {
    // M3-R1 requires an object that never reached connectEstablished() to be
    // rolled back by its selected owner. This narrow invariant makes the old
    // queue-rejection path deterministic without changing the established
    // public shared-owner surface in the same remediation.
    if (state_.load(std::memory_order_relaxed) == kConnecting) {
        if (!loop_->isInLoopThread()) {
            LOG_ERROR
                << "Unestablished TcpConnection released off owner loop: "
                << name_;
            std::terminate();
        }
    }
}

EventLoop* TcpConnection::getLoop() const noexcept {
    return loop_;
}

const std::string& TcpConnection::name() const noexcept {
    return name_;
}

const InetAddress& TcpConnection::localAddress() const noexcept {
    return localAddr_;
}

const InetAddress& TcpConnection::peerAddress() const noexcept {
    return peerAddr_;
}

bool TcpConnection::connected() const noexcept {
    return state_.load(std::memory_order_acquire) == kConnected;
}

bool TcpConnection::disconnected() const noexcept {
    return state_.load(std::memory_order_acquire) == kDisconnected;
}

void TcpConnection::send(std::string_view message) {
    (void)trySend(message.data(), message.size());
}

void TcpConnection::send(const void* data, std::size_t len) {
    (void)trySend(data, len);
}

TcpSendResult TcpConnection::trySend(std::string_view message) {
    return trySend(message.data(), message.size());
}

TcpSendResult TcpConnection::trySend(const void* data, std::size_t len) {
    if (len == 0) {
        return connected() ? TcpSendResult::Accepted : TcpSendResult::Closed;
    }

    if (!connected()) {
        return TcpSendResult::Closed;
    }
    const TcpSendResult reservation = tryReserveOutputBytes(len);
    if (reservation != TcpSendResult::Accepted) {
        return reservation;
    }

    if (loop_->isInLoopThread()) {
        if (state_.load(std::memory_order_relaxed) == kConnected) {
#ifdef _WIN32
            std::string payload;
            try {
                payload.assign(static_cast<const char*>(data), len);
            } catch (...) {
                releaseOutputBytes(len);
                throw;
            }
            sendReservedInLoop(std::move(payload));
#else
            sendReservedInLoop(static_cast<const char*>(data), len);
#endif
            return TcpSendResult::Accepted;
        }
        releaseOutputBytes(len);
        return TcpSendResult::Closed;
    }

    TcpConnectionPtr self;
    try {
        self = shared_from_this();
    } catch (...) {
        releaseOutputBytes(len);
        throw;
    }
    std::string payload;
    try {
        payload.assign(static_cast<const char*>(data), len);
    } catch (...) {
        releaseOutputBytes(len);
        throw;
    }

    bool queued = false;
    try {
        queued = loop_->executor().tryQueue(
            [self, payload = std::move(payload)]() mutable {
                const auto state =
                    self->state_.load(std::memory_order_relaxed);
                if (state == kConnected || state == kDisconnecting) {
#ifdef _WIN32
                    self->sendReservedInLoop(std::move(payload));
#else
                    self->sendReservedInLoop(
                        payload.data(),
                        payload.size());
#endif
                } else {
                    self->releaseOutputBytes(payload.size());
                }
            });
    } catch (...) {
        releaseOutputBytes(len);
        throw;
    }
    if (!queued) {
        releaseOutputBytes(len);
        return TcpSendResult::OwnerUnavailable;
    }
    return TcpSendResult::Accepted;
}

void TcpConnection::shutdown() {
    (void)tryShutdown();
}

void TcpConnection::forceClose() {
    (void)tryForceClose();
}

PostResult TcpConnection::tryShutdown() {
    const StateE state = state_.load(std::memory_order_acquire);
    if (state == kDisconnected) {
        return PostResult::Shutdown;
    }
    publishCloseInfo(TcpConnectionCloseReason::GracefulShutdown);
    gracefulShutdownRequested_.store(true, std::memory_order_release);
    if (loop_->isInLoopThread()) {
        driveLifecycleInLoop();
        return PostResult::Accepted;
    }
    return signalLifecycle();
}

PostResult TcpConnection::tryForceClose() {
    const StateE state = state_.load(std::memory_order_acquire);
    if (state == kDisconnected) {
        return PostResult::Shutdown;
    }
    publishCloseInfo(TcpConnectionCloseReason::ForcedShutdown);
    forceCloseRequested_.store(true, std::memory_order_release);
    if (loop_->isInLoopThread()) {
        driveLifecycleInLoop();
        return PostResult::Accepted;
    }
    return signalLifecycle();
}

void TcpConnection::setTcpNoDelay(bool on) {
    loop_->assertInLoopThread();
    socket_->setTcpNoDelay(on);
}

void TcpConnection::setContext(std::any context) {
    loop_->assertInLoopThread();
    context_ = std::move(context);
}

const std::any& TcpConnection::getContext() const {
    loop_->assertInLoopThread();
    return context_;
}

std::any& TcpConnection::getContext() {
    loop_->assertInLoopThread();
    return context_;
}

void TcpConnection::setConnectionCallback(ConnectionCallback cb) {
    loop_->assertInLoopThread();
    connectionCallback_ = std::move(cb);
}

void TcpConnection::setBackpressureOptions(TcpConnectionBackpressureOptions options) {
    loop_->assertInLoopThread();
    options.validate();
    if (state_.load(std::memory_order_relaxed) != kConnecting) {
        throw std::logic_error(
            "TcpConnection backpressure options must be configured before establishment");
    }
    backpressureOptions_ = options;
}

std::size_t TcpConnection::pendingOutputBytes() const noexcept {
    return pendingOutputBytes_.load(std::memory_order_relaxed);
}

TcpOutputMemoryBudgetSnapshot
TcpConnection::outputMemorySnapshot() const noexcept {
    return TcpOutputMemoryBudgetSnapshot{
        .pendingBytes =
            pendingOutputBytes_.load(std::memory_order_acquire),
        .peakPendingBytes =
            peakPendingOutputBytes_.load(std::memory_order_relaxed),
        .rejectedReservations =
            rejectedOutputReservations_.load(
                std::memory_order_relaxed),
        .overloaded =
            outputAdmissionOverloaded_.load(
                std::memory_order_acquire),
    };
}

TcpConnectionMemoryRetentionSnapshot
TcpConnection::memoryRetentionSnapshot() const {
    loop_->assertInLoopThread();
    TcpConnectionMemoryRetentionSnapshot snapshot{
        .inputBuffer = inputBuffer_.retentionSnapshot(),
        .outputBuffer = outputBuffer_.retentionSnapshot(),
    };
#ifdef _WIN32
    snapshot.transportReadStorageBytes =
        iocpTransport_->readStorageBytes();
    snapshot.peakTransportReadStorageBytes =
        iocpTransport_->peakReadStorageBytes();
#endif
    snapshot.totalRetainedBytes =
        snapshot.inputBuffer.retainedCapacityBytes +
        snapshot.outputBuffer.retainedCapacityBytes +
        snapshot.transportReadStorageBytes;
    return snapshot;
}

std::uint64_t TcpConnection::droppedNotificationCount() const noexcept {
    return droppedNotificationCount_.load(std::memory_order_relaxed);
}

std::optional<TcpConnectionCloseInfo>
TcpConnection::closeInfo() const noexcept {
    const auto bits = closeInfoBits_.load(std::memory_order_acquire);
    if ((bits & kCloseInfoPublished) == 0) {
        return std::nullopt;
    }
    return unpackCloseInfo(bits);
}

TcpConnectionClosePhase TcpConnection::closePhase() const noexcept {
    return closePhase_.load(std::memory_order_acquire);
}

bool TcpConnection::socketClosed() const noexcept {
    const auto phase = closePhase();
    return phase == TcpConnectionClosePhase::SocketClosed ||
        phase == TcpConnectionClosePhase::CompletionDraining ||
        phase == TcpConnectionClosePhase::Closed;
}

bool TcpConnection::readingPausedByBackpressure() const {
    loop_->assertInLoopThread();
    return !backpressure_->readingEnabled();
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    loop_->assertInLoopThread();
    messageCallback_ = std::move(cb);
}

void TcpConnection::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    loop_->assertInLoopThread();
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb) {
    loop_->assertInLoopThread();
    writeCompleteCallback_ = std::move(cb);
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    loop_->assertInLoopThread();
    closeCallback_ = std::move(cb);
}

void TcpConnection::setCloseInfoCallback(CloseInfoCallback cb) {
    loop_->assertInLoopThread();
    closeInfoCallback_ = std::move(cb);
}

void TcpConnection::setCallbackExceptionHandler(
    TcpConnectionCallbackExceptionHandler cb) {
    loop_->assertInLoopThread();
    callbackExceptionHandler_ = std::move(cb);
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) != kConnecting || channelRemoved_) {
        return;
    }
    channel_->tie(shared_from_this());
    channel_->enableReading();
    channelAdded_ = true;
    EventLoopLifecycleSource attachedSource;
    bool lifecycleAttached = false;
    try {
        const auto weakSelf = weak_from_this();
        attachedSource = detail::EventLoopLifecycleRegistry::attach(
            *loop_,
            [weakSelf] {
                if (const auto self = weakSelf.lock()) {
                    self->driveLifecycleInLoop();
                }
            });
        lifecycleAttached = true;
        auto sharedSource =
            std::make_shared<EventLoopLifecycleSource>(attachedSource);
        std::lock_guard lock(lifecycleSourceMutex_);
        lifecycleSource_ = std::move(sharedSource);
    } catch (...) {
        if (lifecycleAttached) {
            detail::EventLoopLifecycleRegistry::detach(
                *loop_,
                attachedSource);
        }
        removeChannelRegistrationInLoop(
            loop_,
            channel_.get(),
            channelAdded_,
            channelRemoved_);
        channelAdded_ = false;
        socket_->close();
        closePhase_.store(
            TcpConnectionClosePhase::Closed,
            std::memory_order_release);
        throw;
    }
    setState(kConnected);
    backpressure_->configure(
        backpressureOptions_.highWaterMarkBytes,
        backpressureOptions_.lowWaterMarkBytes,
        bufferedOutputBytesInLoop(),
        *channel_);
    backpressure_->onConnectionEstablished(
        bufferedOutputBytesInLoop(),
        *channel_);
#ifdef _WIN32
    if (backpressure_->readingEnabled()) {
        const int submitError =
            iocpTransport_->startRead(remainingInputCapacity());
        if (submitError != 0) {
            handleError(submitError);
            return;
        }
    }
#endif

    if (connectionCallback_) {
        auto self = shared_from_this();
        try {
            connectionCallback_(self);
        } catch (...) {
            reportCallbackException(
                TcpConnectionCallbackSource::Established,
                std::current_exception());
            publishCloseInfo(
                TcpConnectionCloseReason::CallbackFailure);
            handleClose();
        }
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    backpressure_->onClosed();
    clearBufferedOutputInLoop();
    const StateE state = state_.load(std::memory_order_relaxed);
    if (state == kConnected || state == kDisconnecting) {
        setState(kDisconnected);
        if (connectionCallback_) {
            try {
                connectionCallback_(shared_from_this());
            } catch (...) {
                reportCallbackException(
                    TcpConnectionCallbackSource::Disconnected,
                    std::current_exception());
            }
        }
    } else {
        setState(kDisconnected);
    }
    if (!channelAdded_) {
        socket_->close();
        closePhase_.store(
            TcpConnectionClosePhase::Closed,
            std::memory_order_release);
        channelRemoved_ = true;
        detachLifecycleNode();
        return;
    }
    removeChannelRegistrationInLoop(
        loop_,
        channel_.get(),
        channelAdded_,
        channelRemoved_);
    detachLifecycleNode();
}

void TcpConnection::handleRead(gamenet::base::Timestamp receiveTime) {
    (void)receiveTime;
    loop_->assertInLoopThread();

    const std::size_t remaining = remainingInputCapacity();
    if (remaining == 0) {
        (void)closeOnInputLimitInLoop();
        return;
    }

    int savedErrno = 0;
#ifdef _WIN32
    const ssize_t n = iocpTransport_->completeRead(&inputBuffer_, &savedErrno);
#else
    const ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno, remaining);
#endif
    if (n > 0) {
        if (backpressure_->readingEnabled() && messageCallback_) {
            try {
                messageCallback_(shared_from_this(), &inputBuffer_);
            } catch (...) {
                reportCallbackException(
                    TcpConnectionCallbackSource::Message,
                    std::current_exception());
                publishCloseInfo(
                    TcpConnectionCloseReason::CallbackFailure);
                handleClose();
                return;
            }
        }
        if (closeOnInputLimitInLoop()) {
            return;
        }
#ifdef _WIN32
        if (state_.load(std::memory_order_relaxed) == kDisconnected) {
            return;
        }
        if (forceClosePending_) {
            handleClose();
            return;
        }
        if (state_.load(std::memory_order_relaxed) == kConnected &&
            backpressure_->readingEnabled()) {
            const int submitError =
                iocpTransport_->startRead(remainingInputCapacity());
            if (submitError != 0) {
                handleError(submitError);
            }
        }
#endif
        return;
    }
    if (n == 0) {
        publishCloseInfo(TcpConnectionCloseReason::PeerEof);
        handleClose();
        return;
    }
    if (sockets::isWouldBlock(savedErrno) || sockets::isInterrupted(savedErrno)) {
        return;
    }
    handleError(savedErrno);
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        return;
    }

    int savedErrno = 0;
#ifdef _WIN32
    const ssize_t n = iocpTransport_->completeWrite(&savedErrno);
#else
    const ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
#endif
    if (n > 0) {
        const auto written = static_cast<std::size_t>(n);
#ifndef _WIN32
        outputBuffer_.retrieve(written);
#endif
        releaseOutputBytes(written);
        applyBackpressureInLoop();
#ifdef _WIN32
        if (state_.load(std::memory_order_relaxed) == kDisconnected) {
            return;
        }
        if (forceClosePending_) {
            handleClose();
            return;
        }
#endif
        if (bufferedOutputBytesInLoop() == 0) {
            channel_->disableWriting();
            if (state_.load(std::memory_order_relaxed) == kDisconnecting) {
                shutdownInLoop();
            }
            queueWriteComplete();
        }
#ifdef _WIN32
        else {
            if (!iocpTransport_->writePending()) {
                const int submitError = iocpTransport_->startWrite();
                if (submitError != 0) {
                    handleError(submitError);
                }
            }
        }
#endif
        return;
    }
    if (n < 0 && !sockets::isWouldBlock(savedErrno) && !sockets::isInterrupted(savedErrno)) {
        handleError(savedErrno);
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        return;
    }
    publishCloseInfo(TcpConnectionCloseReason::PeerEof);
    beginCloseInLoop();
}

void TcpConnection::beginCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        return;
    }
    if (!closeInfo()) {
        publishCloseInfo(TcpConnectionCloseReason::InternalError);
    }

    setState(kDisconnecting);
    auto phase = closePhase_.load(std::memory_order_relaxed);
    if (phase == TcpConnectionClosePhase::Open) {
        closePhase_.store(
            TcpConnectionClosePhase::Closing,
            std::memory_order_release);
        phase = TcpConnectionClosePhase::Closing;
    }

    bool hasPendingOperations = false;
#ifdef _WIN32
    hasPendingOperations = iocpTransport_->hasPendingOperations();
    if (hasPendingOperations) {
        if (!forceClosePending_) {
            forceCloseGuard_ = shared_from_this();
        }
        forceClosePending_ = true;
        iocpTransport_->cancelPendingOperations(channel_->fd());
    }
#endif

#ifndef _WIN32
    // epoll registration bookkeeping is keyed by the numeric descriptor.
    // Revoke the old Channel identity before close() makes that descriptor
    // available to a callback-driven reconnect on the same EventLoop.
    removeChannelRegistrationInLoop(
        loop_,
        channel_.get(),
        channelAdded_,
        channelRemoved_);
#endif

    if (!socketClosed()) {
        if (!hasPendingOperations && !channel_->isNoneEvent()) {
            channel_->disableAll();
        }
        socket_->close();
        closePhase_.store(
            hasPendingOperations
                ? TcpConnectionClosePhase::CompletionDraining
                : TcpConnectionClosePhase::SocketClosed,
            std::memory_order_release);
    }

    if (hasPendingOperations) {
        return;
    }
    if (!channel_->isNoneEvent()) {
        channel_->disableAll();
    }
    closePhase_.store(
        TcpConnectionClosePhase::SocketClosed,
        std::memory_order_release);
    finishClose();
}

void TcpConnection::finishClose() {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        return;
    }

    setState(kDisconnected);
    forceClosePending_ = false;
    backpressure_->onClosed();
#ifdef _WIN32
    iocpTransport_->releaseReadStorage();
#endif
    clearBufferedOutputInLoop();
    if (!socketClosed()) {
        socket_->close();
    }
    if (!channel_->isNoneEvent()) {
        channel_->disableAll();
    }
    closePhase_.store(
        TcpConnectionClosePhase::Closed,
        std::memory_order_release);

    auto self = shared_from_this();
    if (connectionCallback_) {
        try {
            connectionCallback_(self);
        } catch (...) {
            reportCallbackException(
                TcpConnectionCallbackSource::Disconnected,
                std::current_exception());
        }
    }
    if (closeInfoCallback_) {
        try {
            closeInfoCallback_(self, *closeInfo());
        } catch (...) {
            reportCallbackException(
                TcpConnectionCallbackSource::CloseInfo,
                std::current_exception());
        }
    }
    bool closeCallbackFailed = false;
    if (closeCallback_) {
        try {
            closeCallback_(self);
        } catch (...) {
            closeCallbackFailed = true;
            reportCallbackException(
                TcpConnectionCallbackSource::Close,
                std::current_exception());
        }
    }
    forceCloseGuard_.reset();
    if (closeCallbackFailed && !channelRemoved_) {
        connectDestroyed();
    }
}

void TcpConnection::handleError(int savedErrno) {
    loop_->assertInLoopThread();
    const int err = savedErrno != 0 ? savedErrno : sockets::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection error on " << name_ << ": " << err << " " << sockets::errorMessage(err);
    const bool reset =
#ifdef _WIN32
        err == WSAECONNRESET || err == WSAECONNABORTED;
#else
        err == ECONNRESET || err == ECONNABORTED;
#endif
    publishCloseInfo(
        reset
            ? TcpConnectionCloseReason::Reset
            : TcpConnectionCloseReason::InternalError,
        err);
    handleClose();
}

#ifdef _WIN32
void TcpConnection::sendReservedInLoop(std::string payload) {
    loop_->assertInLoopThread();
    const std::size_t len = payload.size();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        releaseOutputBytes(len);
        return;
    }

    const std::size_t oldLen = iocpTransport_->bufferedWriteBytes();
    try {
        iocpTransport_->enqueueWrite(std::move(payload));
    } catch (...) {
        releaseOutputBytes(len);
        throw;
    }
    const std::size_t newLen = iocpTransport_->bufferedWriteBytes();
    applyBackpressureInLoop();
    if (!iocpTransport_->writePending()) {
        const int submitError = iocpTransport_->startWrite();
        if (submitError != 0) {
            handleError(submitError);
            return;
        }
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
    maybeQueueHighWaterMark(oldLen, newLen);
}
#else
void TcpConnection::sendReservedInLoop(
    const char* data,
    std::size_t len) {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        releaseOutputBytes(len);
        return;
    }

    std::size_t remaining = len;
    std::size_t written = 0;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        const ssize_t n = sockets::write(channel_->fd(), data, len);
        if (n >= 0) {
            written = static_cast<std::size_t>(n);
            releaseOutputBytes(written);
            remaining = len - written;
            if (remaining == 0) {
                queueWriteComplete();
                return;
            }
        } else {
            const int err = sockets::lastError();
            if (!sockets::isWouldBlock(err) && !sockets::isInterrupted(err)) {
                releaseOutputBytes(len);
                handleError(err);
                return;
            }
        }
    }

    if (remaining > 0) {
        const std::size_t oldLen = outputBuffer_.readableBytes();
        try {
            outputBuffer_.append(data + written, remaining);
        } catch (...) {
            releaseOutputBytes(remaining);
            throw;
        }
        const std::size_t newLen = outputBuffer_.readableBytes();
        applyBackpressureInLoop();
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        maybeQueueHighWaterMark(oldLen, newLen);
    }
}
#endif

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!socketClosed() && !channel_->isWriting()) {
        socket_->shutdownWrite();
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    const StateE state = state_.load(std::memory_order_relaxed);
    if (state == kConnected || state == kDisconnecting) {
        beginCloseInLoop();
    }
}

void TcpConnection::driveLifecycleInLoop() {
    loop_->assertInLoopThread();
    if (forceCloseRequested_.exchange(false, std::memory_order_acq_rel)) {
        forceCloseInLoop();
    }
    if (gracefulShutdownRequested_.exchange(
            false,
            std::memory_order_acq_rel)) {
        if (state_.load(std::memory_order_relaxed) == kConnected) {
            setState(kDisconnecting);
            shutdownInLoop();
        }
    }
}

void TcpConnection::queueWriteComplete() noexcept {
    if (!writeCompleteCallback_) {
        return;
    }

    try {
        auto cb = writeCompleteCallback_;
        auto self = shared_from_this();
        if (!loop_->tryQueueInLoop([self, cb = std::move(cb)] {
                try {
                    cb(self);
                } catch (...) {
                    self->reportCallbackException(
                        TcpConnectionCallbackSource::WriteComplete,
                        std::current_exception());
                    self->publishCloseInfo(
                        TcpConnectionCloseReason::CallbackFailure);
                    self->handleClose();
                }
            })) {
            recordDroppedNotification();
        }
    } catch (...) {
        recordDroppedNotification();
    }
}

void TcpConnection::maybeQueueHighWaterMark(
    std::size_t oldLen,
    std::size_t newLen) noexcept {
    if (!highWaterMarkCallback_ || highWaterMark_ == 0) {
        return;
    }
    if (oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        try {
            auto cb = highWaterMarkCallback_;
            auto self = shared_from_this();
            if (!loop_->tryQueueInLoop([self, cb = std::move(cb), newLen] {
                    try {
                        cb(self, newLen);
                    } catch (...) {
                        self->reportCallbackException(
                            TcpConnectionCallbackSource::HighWaterMark,
                            std::current_exception());
                        self->publishCloseInfo(
                            TcpConnectionCloseReason::CallbackFailure);
                        self->handleClose();
                    }
                })) {
                recordDroppedNotification();
            }
        } catch (...) {
            recordDroppedNotification();
        }
    }
}

void TcpConnection::recordDroppedNotification() noexcept {
    droppedNotificationCount_.fetch_add(1, std::memory_order_relaxed);
}

TcpSendResult TcpConnection::tryReserveOutputBytes(
    std::size_t bytes) noexcept {
    const std::size_t limit = backpressureOptions_.hardLimitBytes;
    std::size_t current = pendingOutputBytes_.load(std::memory_order_relaxed);
    for (;;) {
        if (outputAdmissionOverloaded_.load(
                std::memory_order_acquire)) {
            if (current > backpressureOptions_.lowWaterMarkBytes) {
                rejectedOutputReservations_.fetch_add(
                    1, std::memory_order_relaxed);
                return TcpSendResult::Overloaded;
            }
            bool expected = true;
            (void)outputAdmissionOverloaded_.compare_exchange_strong(
                expected,
                false,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
            current =
                pendingOutputBytes_.load(std::memory_order_acquire);
            continue;
        }

        if (current > limit || bytes > limit - current) {
            if (current > backpressureOptions_.lowWaterMarkBytes) {
                outputAdmissionOverloaded_.store(
                    true, std::memory_order_release);
            }
            rejectedOutputReservations_.fetch_add(
                1, std::memory_order_relaxed);
            return TcpSendResult::Overloaded;
        }

        if (pendingOutputBytes_.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    if (loopOutputBudget_ &&
        !loopOutputBudget_->tryReserve(bytes)) {
        releaseConnectionOutputBytes(bytes);
        return TcpSendResult::LoopOverloaded;
    }
    if (serverOutputBudget_ &&
        !serverOutputBudget_->tryReserve(bytes)) {
        if (loopOutputBudget_) {
            loopOutputBudget_->release(bytes);
        }
        releaseConnectionOutputBytes(bytes);
        return TcpSendResult::ServerOverloaded;
    }
    if (globalOutputBudget_ &&
        !globalOutputBudget_->tryReserve(bytes)) {
        if (serverOutputBudget_) {
            serverOutputBudget_->release(bytes);
        }
        if (loopOutputBudget_) {
            loopOutputBudget_->release(bytes);
        }
        releaseConnectionOutputBytes(bytes);
        return TcpSendResult::GlobalOverloaded;
    }

    const std::size_t candidate =
        pendingOutputBytes_.load(std::memory_order_relaxed);
    std::size_t peak =
        peakPendingOutputBytes_.load(std::memory_order_relaxed);
    while (peak < candidate &&
           !peakPendingOutputBytes_.compare_exchange_weak(
               peak,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return TcpSendResult::Accepted;
}

void TcpConnection::releaseOutputBytes(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    if (globalOutputBudget_) {
        globalOutputBudget_->release(bytes);
    }
    if (serverOutputBudget_) {
        serverOutputBudget_->release(bytes);
    }
    if (loopOutputBudget_) {
        loopOutputBudget_->release(bytes);
    }
    releaseConnectionOutputBytes(bytes);
}

void TcpConnection::releaseConnectionOutputBytes(
    std::size_t bytes) noexcept {
    const std::size_t previous =
        pendingOutputBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) {
        LOG_FATAL << "TcpConnection output reservation underflow on " << name_;
    }
    if (previous - bytes <= backpressureOptions_.lowWaterMarkBytes) {
        outputAdmissionOverloaded_.store(
            false, std::memory_order_release);
    }
}

void TcpConnection::setOutputMemoryBudgets(
    std::shared_ptr<TcpOutputMemoryBudget> loopBudget,
    std::shared_ptr<TcpOutputMemoryBudget> serverBudget,
    std::shared_ptr<TcpOutputMemoryBudget> globalBudget) {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) != kConnecting) {
        throw std::logic_error(
            "TcpConnection output-memory budgets must be configured before establishment");
    }
    if (!loopBudget || !serverBudget) {
        throw std::invalid_argument(
            "TcpConnection requires loop and server output-memory budgets");
    }
    loopOutputBudget_ = std::move(loopBudget);
    serverOutputBudget_ = std::move(serverBudget);
    globalOutputBudget_ = std::move(globalBudget);
}

std::size_t TcpConnection::bufferedOutputBytesInLoop() const noexcept {
#ifdef _WIN32
    return iocpTransport_->bufferedWriteBytes();
#else
    return outputBuffer_.readableBytes();
#endif
}

void TcpConnection::clearBufferedOutputInLoop() {
    loop_->assertInLoopThread();
    const std::size_t buffered = bufferedOutputBytesInLoop();
    if (buffered == 0) {
        return;
    }
#ifdef _WIN32
    const std::size_t discarded =
        iocpTransport_->discardBufferedWrites();
    if (discarded != buffered) {
        LOG_FATAL << "TcpConnection IOCP write accounting mismatch on "
                  << name_;
    }
#else
    outputBuffer_.retrieveAll();
#endif
    releaseOutputBytes(buffered);
}

void TcpConnection::applyBackpressureInLoop() {
    loop_->assertInLoopThread();
    const bool wasReading = backpressure_->readingEnabled();
    backpressure_->onBufferedBytesChanged(
        bufferedOutputBytesInLoop(),
        *channel_);
#ifdef _WIN32
    if (!wasReading && backpressure_->readingEnabled()) {
        resumeWindowsReadAfterBackpressure();
    }
#else
    (void)wasReading;
#endif
}

std::size_t TcpConnection::remainingInputCapacity() const noexcept {
    const std::size_t buffered = inputBuffer_.readableBytes();
    const std::size_t limit = backpressureOptions_.maxInputBufferBytes;
    return buffered >= limit ? 0 : limit - buffered;
}

bool TcpConnection::closeOnInputLimitInLoop() {
    loop_->assertInLoopThread();
    if (remainingInputCapacity() != 0) {
        return false;
    }
    LOG_WARN << "TcpConnection input buffer limit reached on " << name_ << ": "
             << inputBuffer_.readableBytes() << " bytes";
    publishCloseInfo(TcpConnectionCloseReason::InputLimit);
    handleClose();
    return true;
}

void TcpConnection::publishCloseInfo(
    TcpConnectionCloseReason reason,
    int nativeError) noexcept {
    std::uint64_t expected = 0;
    const std::uint64_t desired = packCloseInfo(reason, nativeError);
    if (closeInfoBits_.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    if (nativeError == 0) {
        return;
    }

    while ((expected & kCloseInfoPublished) != 0 &&
           static_cast<std::uint32_t>(expected) == 0) {
        const auto withNativeError =
            (expected & ~std::uint64_t{0xFFFFFFFFu}) |
            static_cast<std::uint32_t>(nativeError);
        if (closeInfoBits_.compare_exchange_weak(
                expected,
                withNativeError,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

PostResult TcpConnection::signalLifecycle() noexcept {
    std::shared_ptr<EventLoopLifecycleSource> source;
    {
        std::lock_guard lock(lifecycleSourceMutex_);
        source = lifecycleSource_;
    }
    if (!source) {
        return PostResult::OwnerUnavailable;
    }
    return source->signal();
}

void TcpConnection::detachLifecycleNode() {
    loop_->assertInLoopThread();
    std::shared_ptr<EventLoopLifecycleSource> source;
    {
        std::lock_guard lock(lifecycleSourceMutex_);
        source = lifecycleSource_;
    }
    if (!source) {
        return;
    }

    detail::EventLoopLifecycleRegistry::detach(*loop_, *source);
    {
        std::lock_guard lock(lifecycleSourceMutex_);
        if (lifecycleSource_ == source) {
            lifecycleSource_.reset();
        }
    }
}

#ifdef _WIN32
void TcpConnection::resumeWindowsReadAfterBackpressure() {
    loop_->assertInLoopThread();
    auto self = shared_from_this();
    if (state_.load(std::memory_order_relaxed) != kConnected ||
        !backpressure_->readingEnabled()) {
        return;
    }
    if (inputBuffer_.readableBytes() > 0 && messageCallback_) {
        try {
            messageCallback_(self, &inputBuffer_);
        } catch (...) {
            reportCallbackException(
                TcpConnectionCallbackSource::Message,
                std::current_exception());
            publishCloseInfo(
                TcpConnectionCloseReason::CallbackFailure);
            handleClose();
            return;
        }
    }
    if (closeOnInputLimitInLoop()) {
        return;
    }
    if (forceClosePending_ ||
        state_.load(std::memory_order_relaxed) != kConnected ||
        !backpressure_->readingEnabled()) {
        return;
    }
    if (!iocpTransport_->readPending()) {
        const int submitError =
            iocpTransport_->startRead(remainingInputCapacity());
        if (submitError != 0) {
            handleError(submitError);
        }
    }
}
#endif

void TcpConnection::reportCallbackException(
    TcpConnectionCallbackSource source,
    std::exception_ptr exception) noexcept {
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
        LOG_ERROR << "TcpConnection callback on " << name_
                  << " threw an empty exception";
    } catch (const std::exception& error) {
        LOG_ERROR << "TcpConnection callback exception on " << name_
                  << ": " << error.what();
    } catch (...) {
        LOG_ERROR << "TcpConnection callback on " << name_
                  << " threw a non-standard exception";
    }

    if (!callbackExceptionHandler_) {
        return;
    }
    try {
        callbackExceptionHandler_(
            *this,
            TcpConnectionCallbackException{
                .source = source,
                .exception = exception,
            });
    } catch (const std::exception& error) {
        LOG_ERROR << "TcpConnection callback exception observer on " << name_
                  << " threw: " << error.what();
    } catch (...) {
        LOG_ERROR << "TcpConnection callback exception observer on " << name_
                  << " threw a non-standard exception";
    }
}

void TcpConnection::setState(StateE state) noexcept {
    state_.store(state, std::memory_order_release);
}

}  // namespace gamenet::net
