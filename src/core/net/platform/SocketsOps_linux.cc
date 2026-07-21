#include "gamenet/core/net/SocketsOps.h"

#include "gamenet/core/base/Logger.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace gamenet::net::sockets {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << errorMessage(lastError());
    std::abort();
}

void setNonBlockingOrDie(SocketFd sockfd) {
    const int flags = ::fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        die("fcntl(O_NONBLOCK)");
    }
    (void)::fcntl(sockfd, F_SETFD, FD_CLOEXEC);
}

}  // namespace

void ensureInitialized() {
}

SocketFd createNonblockingOrDie(sa_family_t family) {
    const SocketFd sockfd = createNonblocking(family);
    if (!isValid(sockfd)) {
        die("socket");
    }
    return sockfd;
}

SocketFd createNonblocking(sa_family_t family) {
    const SocketFd sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    return sockfd;
}

SocketFd createNonblockingDatagramOrDie(sa_family_t family) {
    const SocketFd sockfd = createNonblockingDatagram(family);
    if (!isValid(sockfd)) {
        die("socket");
    }
    return sockfd;
}

SocketFd createNonblockingDatagram(sa_family_t family) {
    const SocketFd sockfd =
        ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    return sockfd;
}

int bind(SocketFd sockfd, const sockaddr_storage& addr) {
    socklen_t addrLen = 0;
    if (addr.ss_family == AF_INET6) {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in6));
    } else {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    return ::bind(sockfd, reinterpret_cast<const sockaddr*>(&addr), addrLen);
}

void bindOrDie(SocketFd sockfd, const sockaddr_storage& addr) {
    if (bind(sockfd, addr) < 0) {
        die("bind");
    }
}

int listen(SocketFd sockfd) {
    return ::listen(sockfd, SOMAXCONN);
}

void listenOrDie(SocketFd sockfd) {
    if (listen(sockfd) < 0) {
        die("listen");
    }
}

SocketFd accept(SocketFd sockfd, sockaddr_storage* addr) {
    socklen_t addrLen = static_cast<socklen_t>(sizeof(sockaddr_storage));
#ifdef __linux__
    return ::accept4(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const SocketFd connfd = ::accept(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen);
    if (isValid(connfd)) {
        setNonBlockingOrDie(connfd);
    }
    return connfd;
#endif
}

int connect(SocketFd sockfd, const sockaddr* addr, socklen_t addrLen) {
    return ::connect(sockfd, addr, addrLen);
}

void close(SocketFd sockfd) {
    if (!isValid(sockfd)) {
        return;
    }
    if (::close(sockfd) < 0) {
        LOG_SYSERR << "close: " << std::strerror(errno);
    }
}

void shutdownWrite(SocketFd sockfd) {
    if (::shutdown(sockfd, SHUT_WR) < 0) {
        LOG_SYSERR << "shutdown: " << std::strerror(errno);
    }
}

int getSocketError(SocketFd sockfd) {
    int optval = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof(optval));
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return lastError();
    }
    return optval;
}

sockaddr_storage getLocalAddr(SocketFd sockfd) {
    sockaddr_storage localAddr{};
    if (!tryGetLocalAddr(sockfd, &localAddr)) {
        die("getsockname");
    }
    return localAddr;
}

bool tryGetLocalAddr(SocketFd sockfd, sockaddr_storage* result) {
    if (result == nullptr) {
        errno = EINVAL;
        return false;
    }
    socklen_t addrLen = static_cast<socklen_t>(sizeof(*result));
    return ::getsockname(sockfd, reinterpret_cast<sockaddr*>(result), &addrLen) == 0;
}

sockaddr_storage getPeerAddr(SocketFd sockfd) {
    sockaddr_storage peerAddr{};
    (void)tryGetPeerAddr(sockfd, &peerAddr);
    return peerAddr;
}

bool tryGetPeerAddr(SocketFd sockfd, sockaddr_storage* result) {
    if (result == nullptr) {
        errno = EINVAL;
        return false;
    }
    socklen_t addrLen = static_cast<socklen_t>(sizeof(*result));
    return ::getpeername(sockfd, reinterpret_cast<sockaddr*>(result), &addrLen) == 0;
}

ssize_t read(SocketFd sockfd, void* buffer, std::size_t len) {
    return ::read(sockfd, buffer, len);
}

ssize_t write(SocketFd sockfd, const void* buffer, std::size_t len) {
    // A peer can close between readiness observation and the actual write.
    // Keep that recoverable socket error local to the connection instead of
    // letting Linux deliver SIGPIPE and terminate the hosting process.
    const ssize_t n = ::send(sockfd, buffer, len, MSG_NOSIGNAL);
    if (n < 0 && errno == ENOTSOCK) {
        // This compatibility helper is also used with pipes and other file
        // descriptors. Preserve write(2) semantics for those callers while
        // retaining per-call SIGPIPE suppression for sockets.
        return ::write(sockfd, buffer, len);
    }
    return n;
}

ssize_t recvFrom(SocketFd sockfd, void* buffer, std::size_t len, sockaddr_storage* addr, socklen_t* addrLen) {
    return ::recvfrom(sockfd, buffer, len, 0, reinterpret_cast<sockaddr*>(addr), addrLen);
}

ssize_t sendTo(SocketFd sockfd, const void* data, std::size_t len, const sockaddr* addr, socklen_t addrLen) {
    return ::sendto(sockfd, data, len, 0, addr, addrLen);
}

int lastError() noexcept {
    return errno;
}

void setLastError(int error) noexcept {
    errno = error;
}

bool isValid(SocketFd sockfd) noexcept {
    return sockfd != kInvalidSocket;
}

bool isWouldBlock(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool isInterrupted(int error) noexcept {
    return error == EINTR;
}

bool isInProgress(int error) noexcept {
    return error == EINPROGRESS || error == EISCONN;
}

bool isConnectRetryable(int error) noexcept {
    return error == EAGAIN ||
           error == EADDRINUSE ||
           error == EADDRNOTAVAIL ||
           error == ECONNREFUSED ||
           error == ENETUNREACH;
}

bool isMessageSize(int error) noexcept {
    return error == EMSGSIZE;
}

std::string errorMessage(int error) {
    return std::strerror(error);
}

}  // namespace gamenet::net::sockets
