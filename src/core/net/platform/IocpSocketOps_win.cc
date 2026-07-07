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
Fn loadExtension(SocketFd sockfd, GUID guid, const char* name) {
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
    if (rc == SOCKET_ERROR || fn == nullptr) {
        die(name);
    }
    return fn;
}

}  // namespace

SocketFd createOverlappedTcpOrDie(sa_family_t family) {
    sockets::ensureInitialized();
    const SocketFd sockfd = ::WSASocketW(
        family,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);
    if (!sockets::isValid(sockfd)) {
        die("WSASocketW");
    }
    return sockfd;
}

LPFN_ACCEPTEX loadAcceptEx(SocketFd sockfd) {
    GUID guid = WSAID_ACCEPTEX;
    return loadExtension<LPFN_ACCEPTEX>(sockfd, guid, "AcceptEx");
}

LPFN_GETACCEPTEXSOCKADDRS loadGetAcceptExSockaddrs(SocketFd sockfd) {
    GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
    return loadExtension<LPFN_GETACCEPTEXSOCKADDRS>(sockfd, guid, "GetAcceptExSockaddrs");
}

LPFN_CONNECTEX loadConnectEx(SocketFd sockfd) {
    GUID guid = WSAID_CONNECTEX;
    return loadExtension<LPFN_CONNECTEX>(sockfd, guid, "ConnectEx");
}

void updateAcceptContextOrDie(SocketFd accepted, SocketFd listener) {
    if (::setsockopt(
            accepted,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<const char*>(&listener),
            sizeof(listener)) == SOCKET_ERROR) {
        die("SO_UPDATE_ACCEPT_CONTEXT");
    }
}

void updateConnectContextOrDie(SocketFd connected) {
    if (::setsockopt(connected, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR) {
        die("SO_UPDATE_CONNECT_CONTEXT");
    }
}

void bindUnspecifiedOrDie(SocketFd sockfd, sa_family_t family) {
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

    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&storage), len) == SOCKET_ERROR) {
        die("bind unspecified");
    }
}

}  // namespace gamenet::net::platform

#endif
