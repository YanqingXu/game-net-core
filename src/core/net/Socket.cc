#include "gamenet/core/net/Socket.h"

#include "gamenet/core/net/SocketsOps.h"

#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <mstcpip.h>
#else
#include <netinet/tcp.h>
#endif

namespace gamenet::net {

namespace {

void setSocketOption(SocketFd sockfd, int level, int option, bool on) {
#ifdef _WIN32
    const char optval = on ? 1 : 0;
    ::setsockopt(sockfd, level, option, &optval, static_cast<socklen_t>(sizeof(optval)));
#else
    const int optval = on ? 1 : 0;
    ::setsockopt(sockfd, level, option, &optval, static_cast<socklen_t>(sizeof(optval)));
#endif
}

}  // namespace

Socket::Socket(SocketFd sockfd) : sockfd_(sockfd) {
}

Socket::~Socket() {
    sockets::close(sockfd_);
}

SocketFd Socket::fd() const noexcept {
    return sockfd_;
}

SocketFd Socket::releaseFd() noexcept {
    const SocketFd fd = sockfd_;
    sockfd_ = kInvalidSocket;
    return fd;
}

void Socket::bindAddress(const InetAddress& localAddr) {
    int error = 0;
    if (!tryBindAddress(localAddr, &error)) {
        throw std::system_error(
            std::error_code(error, std::system_category()),
            "bind: " + sockets::errorMessage(error));
    }
}

bool Socket::tryBindAddress(const InetAddress& localAddr, int* error) {
    sockaddr_storage storage{};
    std::memcpy(&storage, localAddr.getSockAddr(), localAddr.getSockAddrLen());
    if (sockets::bind(sockfd_, storage) == 0) {
        return true;
    }
    if (error != nullptr) {
        *error = sockets::lastError();
    }
    return false;
}

void Socket::listen() {
    int error = 0;
    if (!tryListen(&error)) {
        throw std::system_error(
            std::error_code(error, std::system_category()),
            "listen: " + sockets::errorMessage(error));
    }
}

bool Socket::tryListen(int* error) {
    if (sockets::listen(sockfd_) == 0) {
        return true;
    }
    if (error != nullptr) {
        *error = sockets::lastError();
    }
    return false;
}

SocketFd Socket::accept(InetAddress* peerAddr) {
    sockaddr_storage addr{};
    const SocketFd connfd = sockets::accept(sockfd_, &addr);
    if (sockets::isValid(connfd) && peerAddr != nullptr) {
        if (addr.ss_family == AF_INET6) {
            peerAddr->setSockAddrInet6(*reinterpret_cast<const sockaddr_in6*>(&addr));
        } else {
            peerAddr->setSockAddrInet(*reinterpret_cast<const sockaddr_in*>(&addr));
        }
    }
    return connfd;
}

void Socket::shutdownWrite() {
    sockets::shutdownWrite(sockfd_);
}

void Socket::setReuseAddr(bool on) {
    setSocketOption(sockfd_, SOL_SOCKET, SO_REUSEADDR, on);
}

void Socket::setReusePort(bool on) {
#ifdef SO_REUSEPORT
    setSocketOption(sockfd_, SOL_SOCKET, SO_REUSEPORT, on);
#else
    (void)on;
#endif
}

void Socket::setKeepAlive(bool on) {
    setSocketOption(sockfd_, SOL_SOCKET, SO_KEEPALIVE, on);
}

void Socket::setTcpNoDelay(bool on) {
    setSocketOption(sockfd_, IPPROTO_TCP, TCP_NODELAY, on);
}

void Socket::setIpv6Only(bool on) {
    setSocketOption(sockfd_, IPPROTO_IPV6, IPV6_V6ONLY, on);
}

}  // namespace gamenet::net
