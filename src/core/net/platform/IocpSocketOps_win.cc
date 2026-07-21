#include "gamenet/core/net/platform/IocpSocketOps.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cstdlib>

namespace gamenet::net::platform {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << sockets::errorMessage(sockets::lastError());
    std::abort();
}

template <typename Fn>
Fn loadExtension(SocketFd sockfd, GUID guid) {
    Fn fn = nullptr;
    DWORD bytes = 0;
    const int rc = ::WSAIoctl(
        sockfd,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid,
        sizeof(guid),
        &fn,
        sizeof(fn),
        &bytes,
        nullptr,
        nullptr);
    return rc == SOCKET_ERROR ? nullptr : fn;
}

}  // namespace

SocketFd createOverlappedTcpOrDie(sa_family_t family) {
    const SocketFd sockfd = createOverlappedTcp(family);
    if (!sockets::isValid(sockfd)) {
        die("WSASocketW");
    }
    return sockfd;
}

SocketFd createOverlappedTcp(sa_family_t family) {
    sockets::ensureInitialized();
    return ::WSASocketW(
        family,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);
}

LPFN_ACCEPTEX loadAcceptEx(SocketFd sockfd) {
    GUID guid = WSAID_ACCEPTEX;
    return loadExtension<LPFN_ACCEPTEX>(sockfd, guid);
}

LPFN_GETACCEPTEXSOCKADDRS loadGetAcceptExSockaddrs(SocketFd sockfd) {
    GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
    return loadExtension<LPFN_GETACCEPTEXSOCKADDRS>(sockfd, guid);
}

LPFN_CONNECTEX loadConnectEx(SocketFd sockfd) {
    GUID guid = WSAID_CONNECTEX;
    return loadExtension<LPFN_CONNECTEX>(sockfd, guid);
}

void updateAcceptContextOrDie(SocketFd accepted, SocketFd listener) {
    if (!updateAcceptContext(accepted, listener)) {
        die("SO_UPDATE_ACCEPT_CONTEXT");
    }
}

bool updateAcceptContext(SocketFd accepted, SocketFd listener) {
    return ::setsockopt(
            accepted,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<const char*>(&listener),
            sizeof(listener)) != SOCKET_ERROR;
}

void updateConnectContextOrDie(SocketFd connected) {
    if (!updateConnectContext(connected)) {
        die("SO_UPDATE_CONNECT_CONTEXT");
    }
}

bool updateConnectContext(SocketFd connected) {
    return ::setsockopt(connected, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) !=
        SOCKET_ERROR;
}

void bindUnspecifiedOrDie(SocketFd sockfd, sa_family_t family) {
    if (!bindUnspecified(sockfd, family)) {
        die("bind unspecified");
    }
}

bool bindUnspecified(SocketFd sockfd, sa_family_t family) {
    sockaddr_storage storage{};
    int len = 0;
    if (family == AF_INET6) {
        auto& addr = *reinterpret_cast<sockaddr_in6*>(&storage);
        addr.sin6_family = AF_INET6;
        len = static_cast<int>(sizeof(sockaddr_in6));
    } else {
        auto& addr = *reinterpret_cast<sockaddr_in*>(&storage);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        len = static_cast<int>(sizeof(sockaddr_in));
    }

    return ::bind(sockfd, reinterpret_cast<sockaddr*>(&storage), len) != SOCKET_ERROR;
}

}  // namespace gamenet::net::platform

#endif
