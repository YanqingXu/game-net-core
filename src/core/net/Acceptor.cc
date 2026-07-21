#include "gamenet/core/net/Acceptor.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "gamenet/core/base/Logger.h"

#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#include "gamenet/core/net/platform/IocpSocketOps.h"

#include <array>
#endif

#include <cerrno>
#include <chrono>
#include <system_error>
#include <utility>

namespace gamenet::net {

namespace {

[[noreturn]] void throwSocketError(const char* operation, int error) {
    throw std::system_error(
        std::error_code(error, std::system_category()),
        std::string(operation) + ": " + sockets::errorMessage(error));
}

SocketFd createAcceptSocket(sa_family_t family) {
    const SocketFd fd = sockets::createNonblocking(family);
    if (!sockets::isValid(fd)) {
        throwSocketError("accept socket creation", sockets::lastError());
    }
    return fd;
}

}  // namespace

#ifdef _WIN32

namespace {

constexpr DWORD kAcceptAddressBytes = sizeof(sockaddr_storage) + 16;

}  // namespace

struct Acceptor::IocpAcceptState {
    IocpOperation operation{};
    SocketFd accepted{kInvalidSocket};
    std::array<char, kAcceptAddressBytes * 2> addresses{};
    LPFN_ACCEPTEX acceptEx{nullptr};
    LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs{nullptr};
    bool pending{false};

    IocpAcceptState() {
        operation.kind = IocpOperationKind::Accept;
    }
};

#endif

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(createAcceptSocket(listenAddr.family())),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reusePort);
    // For IPv6 sockets, default to dual-stack (IPV6_V6ONLY=0) so the same
    // socket can accept both IPv4-mapped and native IPv6 connections.
    if (listenAddr.isIpv6()) {
        acceptSocket_.setIpv6Only(false);
    }
    acceptSocket_.bindAddress(listenAddr);
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(acceptSocket_.fd(), &localStorage)) {
        throwSocketError("getsockname", sockets::lastError());
    }
    listenAddr_ = InetAddress(localStorage);
    acceptChannel_.setReadCallback([this](gamenet::base::Timestamp receiveTime) { handleRead(receiveTime); });
}

Acceptor::~Acceptor() {
    if (!loop_->isInLoopThread()) {
        LOG_FATAL << "Acceptor destroyed from non-owner thread";
    }
    if (retryTimer_.valid()) {
        loop_->cancel(retryTimer_);
        retryTimer_ = {};
    }
#ifdef _WIN32
    closePendingAccept();
#endif
    if (listening_) {
        acceptChannel_.disableAll();
        acceptChannel_.remove();
    }
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
}

void Acceptor::setErrorCallback(AcceptorErrorCallback cb) {
    errorCallback_ = std::move(cb);
}

bool Acceptor::listening() const noexcept {
    return listening_;
}

const InetAddress& Acceptor::listenAddress() const noexcept {
    return listenAddr_;
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    acceptSocket_.listen();
#ifdef _WIN32
    if (!iocpAccept_) {
        iocpAccept_ = std::make_shared<IocpAcceptState>();
        iocpAccept_->acceptEx = platform::loadAcceptEx(acceptSocket_.fd());
        if (iocpAccept_->acceptEx == nullptr) {
            throwSocketError("load AcceptEx", sockets::lastError());
        }
        iocpAccept_->getAcceptExSockaddrs = platform::loadGetAcceptExSockaddrs(acceptSocket_.fd());
        if (iocpAccept_->getAcceptExSockaddrs == nullptr) {
            throwSocketError("load GetAcceptExSockaddrs", sockets::lastError());
        }
    }
    iocpAccept_->operation.channel = &acceptChannel_;
#endif
    listening_ = true;
    acceptChannel_.enableReading();
#ifdef _WIN32
    postAccept();
#endif
}

void Acceptor::stop() {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    listening_ = false;
    if (retryTimer_.valid()) {
        loop_->cancel(retryTimer_);
        retryTimer_ = {};
    }
#ifdef _WIN32
    closePendingAccept();
#endif
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    // Close the listen socket so the kernel rejects further connections.
    // Release the fd from the Socket RAII wrapper first to avoid double-close.
    const SocketFd fd = acceptSocket_.releaseFd();
    if (sockets::isValid(fd)) {
        sockets::close(fd);
    }
}

void Acceptor::handleRead(gamenet::base::Timestamp receiveTime) {
    (void)receiveTime;
    loop_->assertInLoopThread();

#ifdef _WIN32
    if (!iocpAccept_ || !iocpAccept_->pending) {
        return;
    }
    iocpAccept_->pending = false;
    if (iocpAccept_->operation.error != 0) {
        const int error = static_cast<int>(iocpAccept_->operation.error);
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        handleAcceptError(AcceptorErrorStage::Accept, error);
        return;
    }

    if (!platform::updateAcceptContext(iocpAccept_->accepted, acceptSocket_.fd())) {
        const int error = sockets::lastError();
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        handleAcceptError(AcceptorErrorStage::AcceptedSocketSetup, error);
        return;
    }
    sockaddr_storage peerStorage{};
    if (!sockets::tryGetPeerAddr(iocpAccept_->accepted, &peerStorage)) {
        const int error = sockets::lastError();
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        handleAcceptError(AcceptorErrorStage::AcceptedSocketSetup, error);
        return;
    }
    InetAddress peerAddr(peerStorage);
    const SocketFd connfd = iocpAccept_->accepted;
    iocpAccept_->accepted = kInvalidSocket;

    if (newConnectionCallback_) {
        newConnectionCallback_(connfd, peerAddr);
    } else {
        sockets::close(connfd);
    }

    if (listening_) {
        postAccept();
    }
    return;
#else
    while (true) {
        InetAddress peerAddr;
        const SocketFd connfd = acceptSocket_.accept(&peerAddr);
        if (sockets::isValid(connfd)) {
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);
            } else {
                sockets::close(connfd);
            }
            continue;
        }

        const int error = sockets::lastError();
        if (sockets::isWouldBlock(error)) {
            break;
        }
        if (sockets::isInterrupted(error)) {
            continue;
        }
        handleAcceptError(AcceptorErrorStage::Accept, error);
        break;
    }
#endif
}

void Acceptor::handleAcceptError(AcceptorErrorStage stage, int error) {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    LOG_WARN << "Acceptor runtime error: " << error << " " << sockets::errorMessage(error);
    AcceptorErrorAction action = AcceptorErrorAction::Retry;
    if (errorCallback_) {
        try {
            action = errorCallback_(AcceptorError{.stage = stage, .errorCode = error});
        } catch (const std::exception& callbackError) {
            LOG_ERROR << "Acceptor error callback threw: " << callbackError.what();
            action = AcceptorErrorAction::Stop;
        } catch (...) {
            LOG_ERROR << "Acceptor error callback threw a non-standard exception";
            action = AcceptorErrorAction::Stop;
        }
    }
    if (action == AcceptorErrorAction::Stop) {
        stop();
        return;
    }
    scheduleAcceptRetry();
}

void Acceptor::scheduleAcceptRetry() {
    loop_->assertInLoopThread();
    if (!listening_ || retryTimer_.valid()) {
        return;
    }
    if (acceptChannel_.isReading()) {
        acceptChannel_.disableReading();
    }
    retryTimer_ = loop_->runAfter(std::chrono::milliseconds(100), [this] { resumeAccept(); });
}

void Acceptor::resumeAccept() {
    loop_->assertInLoopThread();
    retryTimer_ = {};
    if (!listening_) {
        return;
    }
    if (!acceptChannel_.isReading()) {
        acceptChannel_.enableReading();
    }
#ifdef _WIN32
    postAccept();
#endif
}

#ifdef _WIN32

void Acceptor::postAccept() {
    loop_->assertInLoopThread();
    if (!listening_ || !iocpAccept_ || iocpAccept_->pending) {
        return;
    }

    iocpAccept_->accepted = platform::createOverlappedTcp(listenAddr_.family());
    if (!sockets::isValid(iocpAccept_->accepted)) {
        handleAcceptError(AcceptorErrorStage::AcceptedSocketCreate, sockets::lastError());
        return;
    }
    iocpAccept_->operation.overlapped = OVERLAPPED{};
    iocpAccept_->operation.kind = IocpOperationKind::Accept;
    iocpAccept_->operation.channel = &acceptChannel_;
    iocpAccept_->operation.bytesTransferred = 0;
    iocpAccept_->operation.error = 0;

    DWORD bytes = 0;
    iocpAccept_->pending = true;
    const BOOL ok = iocpAccept_->acceptEx(
        acceptSocket_.fd(),
        iocpAccept_->accepted,
        iocpAccept_->addresses.data(),
        0,
        kAcceptAddressBytes,
        kAcceptAddressBytes,
        &bytes,
        &iocpAccept_->operation.overlapped);
    if (!ok && sockets::lastError() != ERROR_IO_PENDING) {
        const int error = sockets::lastError();
        iocpAccept_->pending = false;
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        handleAcceptError(AcceptorErrorStage::Accept, error);
        return;
    }
    loop_->retainCompletionOperation(&iocpAccept_->operation, iocpAccept_);
}

void Acceptor::closePendingAccept() noexcept {
    if (!iocpAccept_) {
        return;
    }
    iocpAccept_->pending = false;
    iocpAccept_->operation.channel = nullptr;
    if (sockets::isValid(iocpAccept_->accepted)) {
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
    }
}

#endif

}  // namespace gamenet::net
