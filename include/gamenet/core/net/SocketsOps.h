#pragma once

// SocketsOps 暴露最底层的 socket 系统调用辅助函数。
// IPv4/IPv6 双栈：createNonblocking/createNonblockingOrDie 接受 family 参数，
// bind/accept/getLocalAddr/getPeerAddr 统一使用 sockaddr_storage。

#include "gamenet/core/net/SocketTypes.h"

#include <cstddef>
#include <string>

namespace gamenet::net::sockets {

void ensureInitialized();

/// Create a non-blocking, close-on-exec TCP socket for the given address family.
/// @param family  AF_INET or AF_INET6
SocketFd createNonblocking(sa_family_t family);
SocketFd createNonblockingDatagram(sa_family_t family);
SocketFd createNonblockingOrDie(sa_family_t family);
SocketFd createNonblockingDatagramOrDie(sa_family_t family);

// Address/output pointers are borrowed for the call. Non-null pointers must
// address suitably aligned storage of the stated type. connect() additionally
// requires addrLen bytes readable from addr; accept/tryGet write their result.
int bind(SocketFd sockfd, const sockaddr_storage& addr);
int listen(SocketFd sockfd);
void bindOrDie(SocketFd sockfd, const sockaddr_storage& addr);
void listenOrDie(SocketFd sockfd);
// On success, the returned descriptor is uniquely caller-owned and must be
// closed or transferred to RAII ownership. Failure returns kInvalidSocket and
// transfers nothing; addr is borrowed writable output storage and may be null.
SocketFd accept(SocketFd sockfd, sockaddr_storage* addr);
int connect(SocketFd sockfd, const sockaddr* addr, socklen_t addrLen);
void close(SocketFd sockfd);
void shutdownWrite(SocketFd sockfd);
int getSocketError(SocketFd sockfd);
sockaddr_storage getLocalAddr(SocketFd sockfd);
bool tryGetLocalAddr(SocketFd sockfd, sockaddr_storage* result);
sockaddr_storage getPeerAddr(SocketFd sockfd);
bool tryGetPeerAddr(SocketFd sockfd, sockaddr_storage* result);

// Pointer arguments are borrowed for the call only. For len > 0, read/recv
// buffers must provide writable [buffer, buffer + len) storage and write/send
// buffers readable [buffer, buffer + len) storage; zero length permits null.
// Address pointers and lengths must describe storage valid for the underlying
// platform socket call; result/out-length pointers must be writable.
ssize_t read(SocketFd sockfd, void* buffer, std::size_t len);
ssize_t write(SocketFd sockfd, const void* buffer, std::size_t len);
ssize_t recvFrom(SocketFd sockfd, void* buffer, std::size_t len, sockaddr_storage* addr, socklen_t* addrLen);
ssize_t sendTo(SocketFd sockfd, const void* data, std::size_t len, const sockaddr* addr, socklen_t addrLen);

int lastError() noexcept;
void setLastError(int error) noexcept;
bool isValid(SocketFd sockfd) noexcept;
bool isWouldBlock(int error) noexcept;
bool isInterrupted(int error) noexcept;
bool isInProgress(int error) noexcept;
bool isConnectRetryable(int error) noexcept;
bool isMessageSize(int error) noexcept;
std::string errorMessage(int error);

#ifdef _WIN32
void createSocketPairOrDie(SocketFd fds[2]);
#endif

}  // namespace gamenet::net::sockets
