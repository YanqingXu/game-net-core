#pragma once

#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32

#include <mswsock.h>
#include <winsock2.h>

namespace gamenet::net::platform {

SocketFd createOverlappedTcpOrDie(sa_family_t family);
SocketFd createOverlappedTcp(sa_family_t family);
LPFN_ACCEPTEX loadAcceptEx(SocketFd sockfd);
LPFN_GETACCEPTEXSOCKADDRS loadGetAcceptExSockaddrs(SocketFd sockfd);
LPFN_CONNECTEX loadConnectEx(SocketFd sockfd);
void updateAcceptContextOrDie(SocketFd accepted, SocketFd listener);
bool updateAcceptContext(SocketFd accepted, SocketFd listener);
void updateConnectContextOrDie(SocketFd connected);
bool updateConnectContext(SocketFd connected);
void bindUnspecifiedOrDie(SocketFd sockfd, sa_family_t family);
bool bindUnspecified(SocketFd sockfd, sa_family_t family);

}  // namespace gamenet::net::platform

#endif
