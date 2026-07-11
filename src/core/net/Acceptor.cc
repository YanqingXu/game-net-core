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
#include <utility>

namespace gamenet::net {

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
      acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
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
    listenAddr_ = InetAddress(sockets::getLocalAddr(acceptSocket_.fd()));
    acceptChannel_.setReadCallback([this](gamenet::base::Timestamp receiveTime) { handleRead(receiveTime); });
}

Acceptor::~Acceptor() {
    if (!loop_->isInLoopThread()) {
        LOG_FATAL << "Acceptor destroyed from non-owner thread";
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

bool Acceptor::listening() const noexcept {
    return listening_;
}

const InetAddress& Acceptor::listenAddress() const noexcept {
    return listenAddr_;
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
#ifdef _WIN32
    if (!iocpAccept_) {
        iocpAccept_ = std::make_shared<IocpAcceptState>();
        iocpAccept_->acceptEx = platform::loadAcceptEx(acceptSocket_.fd());
        iocpAccept_->getAcceptExSockaddrs = platform::loadGetAcceptExSockaddrs(acceptSocket_.fd());
    }
    iocpAccept_->operation.channel = &acceptChannel_;
    postAccept();
#endif
}

void Acceptor::stop() {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    listening_ = false;
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
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        if (listening_) {
            postAccept();
        }
        return;
    }

    platform::updateAcceptContextOrDie(iocpAccept_->accepted, acceptSocket_.fd());
    InetAddress peerAddr(sockets::getPeerAddr(iocpAccept_->accepted));
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
        break;
    }
#endif
}

#ifdef _WIN32

void Acceptor::postAccept() {
    loop_->assertInLoopThread();
    if (!listening_ || !iocpAccept_ || iocpAccept_->pending) {
        return;
    }

    iocpAccept_->accepted = platform::createOverlappedTcpOrDie(listenAddr_.family());
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
        iocpAccept_->pending = false;
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        LOG_SYSFATAL << "AcceptEx: " << sockets::errorMessage(sockets::lastError());
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
