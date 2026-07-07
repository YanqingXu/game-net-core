#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "platform/IocpTcpTransport.h"
#endif

#include <utility>

namespace gamenet::net {

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
      localAddr_(localAddr),
      peerAddr_(peerAddr) {
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
    return state_ == kConnected;
}

bool TcpConnection::disconnected() const noexcept {
    return state_ == kDisconnected;
}

void TcpConnection::send(std::string_view message) {
    send(message.data(), message.size());
}

void TcpConnection::send(const void* data, std::size_t len) {
    if (len == 0) {
        return;
    }

    if (loop_->isInLoopThread()) {
        if (state_ == kConnected) {
            sendInLoop(static_cast<const char*>(data), len);
        }
        return;
    }

    auto self = shared_from_this();
    std::string payload(static_cast<const char*>(data), len);
    loop_->runInLoop([self, payload = std::move(payload)] {
        if (self->connected()) {
            self->sendInLoop(payload.data(), payload.size());
        }
    });
}

void TcpConnection::shutdown() {
    auto self = shared_from_this();
    loop_->runInLoop([self] {
        if (self->state_ == kConnected) {
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
    connectionCallback_ = std::move(cb);
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpConnection::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    if (state_ != kConnecting || channelRemoved_) {
        return;
    }
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();
    channelAdded_ = true;
#ifdef _WIN32
    iocpTransport_->startRead();
#endif

    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected || state_ == kDisconnecting) {
        setState(kDisconnected);
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
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

    int savedErrno = 0;
#ifdef _WIN32
    const ssize_t n = iocpTransport_->completeRead(&inputBuffer_, &savedErrno);
#else
    const ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
#endif
    if (n > 0) {
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
#ifdef _WIN32
        if (state_ == kConnected) {
            iocpTransport_->startRead();
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
        outputBuffer_.retrieve(static_cast<std::size_t>(n));
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();
            queueWriteComplete();
            if (state_ == kDisconnecting) {
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
    if (state_ == kDisconnected) {
        return;
    }

    setState(kDisconnected);
    channel_->disableAll();

    auto self = shared_from_this();
    if (connectionCallback_) {
        connectionCallback_(self);
    }
    if (closeCallback_) {
        closeCallback_(self);
    }
}

void TcpConnection::handleError(int savedErrno) {
    loop_->assertInLoopThread();
    const int err = savedErrno != 0 ? savedErrno : sockets::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection error on " << name_ << ": " << err << " " << sockets::errorMessage(err);
    handleClose();
}

void TcpConnection::sendInLoop(const char* data, std::size_t len) {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) {
        return;
    }

#ifdef _WIN32
    const std::size_t oldLen = outputBuffer_.readableBytes();
    outputBuffer_.append(data, len);
    const std::size_t newLen = outputBuffer_.readableBytes();
    maybeQueueHighWaterMark(oldLen, newLen);
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
            remaining = len - written;
            if (remaining == 0) {
                queueWriteComplete();
                return;
            }
        } else {
            const int err = sockets::lastError();
            if (!sockets::isWouldBlock(err) && !sockets::isInterrupted(err)) {
                handleError(err);
                return;
            }
        }
    }

    if (remaining > 0) {
        const std::size_t oldLen = outputBuffer_.readableBytes();
        outputBuffer_.append(data + written, remaining);
        const std::size_t newLen = outputBuffer_.readableBytes();
        maybeQueueHighWaterMark(oldLen, newLen);
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
    if (state_ == kConnected || state_ == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::queueWriteComplete() {
    if (!writeCompleteCallback_) {
        return;
    }
    auto cb = writeCompleteCallback_;
    auto self = shared_from_this();
    loop_->queueInLoop([self, cb = std::move(cb)] { cb(self); });
}

void TcpConnection::maybeQueueHighWaterMark(std::size_t oldLen, std::size_t newLen) {
    if (!highWaterMarkCallback_ || highWaterMark_ == 0) {
        return;
    }
    if (oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        auto cb = highWaterMarkCallback_;
        auto self = shared_from_this();
        loop_->queueInLoop([self, cb = std::move(cb), newLen] { cb(self, newLen); });
    }
}

void TcpConnection::setState(StateE state) noexcept {
    state_ = state;
}

}  // namespace gamenet::net
