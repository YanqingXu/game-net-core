#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "detail/ConnectionBackpressureController.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "platform/IocpTcpTransport.h"
#endif

#include <stdexcept>
#include <utility>

namespace gamenet::net {

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

TcpConnection::~TcpConnection() = default;

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
    return state_.load(std::memory_order_relaxed) == kConnected;
}

bool TcpConnection::disconnected() const noexcept {
    return state_.load(std::memory_order_relaxed) == kDisconnected;
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
    if (!tryReserveOutputBytes(len)) {
        return TcpSendResult::Overloaded;
    }

    if (loop_->isInLoopThread()) {
        if (state_.load(std::memory_order_relaxed) == kConnected) {
            sendReservedInLoop(static_cast<const char*>(data), len);
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
        queued = loop_->executor().tryQueue([self, payload = std::move(payload)] {
            const auto state = self->state_.load(std::memory_order_relaxed);
            if (state == kConnected || state == kDisconnecting) {
                self->sendReservedInLoop(payload.data(), payload.size());
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
    auto self = shared_from_this();
    loop_->runInLoop([self] {
        if (self->state_.load(std::memory_order_relaxed) == kConnected) {
            self->setState(kDisconnecting);
            self->shutdownInLoop();
        }
    });
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    loop_->runInLoop([self] { self->forceCloseInLoop(); });
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
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();
    channelAdded_ = true;
    backpressure_->configure(
        backpressureOptions_.highWaterMarkBytes,
        backpressureOptions_.lowWaterMarkBytes,
        outputBuffer_.readableBytes(),
        *channel_);
    backpressure_->onConnectionEstablished(outputBuffer_.readableBytes(), *channel_);
#ifdef _WIN32
    if (backpressure_->readingEnabled()) {
        iocpTransport_->startRead(remainingInputCapacity());
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
        channelRemoved_ = true;
        return;
    }
    if (!channel_->isNoneEvent()) {
        channel_->disableAll();
    }
    if (!channelRemoved_) {
        channel_->remove();
        channelRemoved_ = true;
    }
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
                handleClose();
                return;
            }
        }
        if (closeOnInputLimitInLoop()) {
            return;
        }
#ifdef _WIN32
        if (forceClosePending_) {
            handleClose();
            return;
        }
        if (state_.load(std::memory_order_relaxed) == kConnected &&
            backpressure_->readingEnabled()) {
            iocpTransport_->startRead(remainingInputCapacity());
        }
#endif
        return;
    }
    if (n == 0) {
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
        outputBuffer_.retrieve(written);
        releaseOutputBytes(written);
        applyBackpressureInLoop();
#ifdef _WIN32
        if (forceClosePending_) {
            handleClose();
            return;
        }
#endif
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();
            queueWriteComplete();
            if (state_.load(std::memory_order_relaxed) == kDisconnecting) {
                shutdownInLoop();
            }
        }
#ifdef _WIN32
        else {
            iocpTransport_->startWrite(outputBuffer_.peek(), outputBuffer_.readableBytes());
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

#ifdef _WIN32
    if (iocpTransport_->hasPendingOperations()) {
        if (!forceClosePending_) {
            forceCloseGuard_ = shared_from_this();
        }
        forceClosePending_ = true;
        setState(kDisconnecting);
        iocpTransport_->cancelPendingOperations(channel_->fd());
        return;
    }
#endif

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
    clearBufferedOutputInLoop();
    channel_->disableAll();

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
    handleClose();
}

void TcpConnection::sendReservedInLoop(const char* data, std::size_t len) {
    loop_->assertInLoopThread();
    if (state_.load(std::memory_order_relaxed) == kDisconnected) {
        releaseOutputBytes(len);
        return;
    }

#ifdef _WIN32
    const std::size_t oldLen = outputBuffer_.readableBytes();
    try {
        outputBuffer_.append(data, len);
    } catch (...) {
        releaseOutputBytes(len);
        throw;
    }
    const std::size_t newLen = outputBuffer_.readableBytes();
    maybeQueueHighWaterMark(oldLen, newLen);
    applyBackpressureInLoop();
    if (!iocpTransport_->writePending()) {
        iocpTransport_->startWrite(outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
    return;
#endif

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
        maybeQueueHighWaterMark(oldLen, newLen);
        applyBackpressureInLoop();
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        socket_->shutdownWrite();
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    const StateE state = state_.load(std::memory_order_relaxed);
    if (state == kConnected || state == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::queueWriteComplete() {
    if (!writeCompleteCallback_) {
        return;
    }
    auto cb = writeCompleteCallback_;
    auto self = shared_from_this();
    loop_->queueInLoop([self, cb = std::move(cb)] {
        try {
            cb(self);
        } catch (...) {
            self->reportCallbackException(
                TcpConnectionCallbackSource::WriteComplete,
                std::current_exception());
            self->handleClose();
        }
    });
}

void TcpConnection::maybeQueueHighWaterMark(std::size_t oldLen, std::size_t newLen) {
    if (!highWaterMarkCallback_ || highWaterMark_ == 0) {
        return;
    }
    if (oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        auto cb = highWaterMarkCallback_;
        auto self = shared_from_this();
        loop_->queueInLoop([self, cb = std::move(cb), newLen] {
            try {
                cb(self, newLen);
            } catch (...) {
                self->reportCallbackException(
                    TcpConnectionCallbackSource::HighWaterMark,
                    std::current_exception());
                self->handleClose();
            }
        });
    }
}

bool TcpConnection::tryReserveOutputBytes(std::size_t bytes) noexcept {
    const std::size_t limit = backpressureOptions_.hardLimitBytes;
    std::size_t current = pendingOutputBytes_.load(std::memory_order_relaxed);
    while (current <= limit && bytes <= limit - current) {
        if (pendingOutputBytes_.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void TcpConnection::releaseOutputBytes(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::size_t previous =
        pendingOutputBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) {
        LOG_FATAL << "TcpConnection output reservation underflow on " << name_;
    }
}

void TcpConnection::clearBufferedOutputInLoop() {
    loop_->assertInLoopThread();
    const std::size_t buffered = outputBuffer_.readableBytes();
    if (buffered == 0) {
        return;
    }
    outputBuffer_.retrieveAll();
    releaseOutputBytes(buffered);
}

void TcpConnection::applyBackpressureInLoop() {
    loop_->assertInLoopThread();
    const bool wasReading = backpressure_->readingEnabled();
    backpressure_->onBufferedBytesChanged(outputBuffer_.readableBytes(), *channel_);
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
    handleClose();
    return true;
}

#ifdef _WIN32
void TcpConnection::resumeWindowsReadAfterBackpressure() {
    loop_->assertInLoopThread();
    auto self = shared_from_this();
    loop_->queueInLoop([self] {
        if (self->state_.load(std::memory_order_relaxed) != kConnected ||
            !self->backpressure_->readingEnabled()) {
            return;
        }
        if (self->inputBuffer_.readableBytes() > 0 && self->messageCallback_) {
            try {
                self->messageCallback_(self, &self->inputBuffer_);
            } catch (...) {
                self->reportCallbackException(
                    TcpConnectionCallbackSource::Message,
                    std::current_exception());
                self->handleClose();
                return;
            }
        }
        if (self->closeOnInputLimitInLoop()) {
            return;
        }
        if (self->forceClosePending_ ||
            self->state_.load(std::memory_order_relaxed) != kConnected ||
            !self->backpressure_->readingEnabled()) {
            return;
        }
        if (!self->iocpTransport_->readPending()) {
            self->iocpTransport_->startRead(self->remainingInputCapacity());
        }
    });
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
    state_.store(state, std::memory_order_relaxed);
}

}  // namespace gamenet::net
