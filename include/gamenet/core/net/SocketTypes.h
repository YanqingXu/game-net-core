#pragma once

// Stable cross-platform socket handle and system-call type surface.
// Operating-system backend headers may depend on these declarations, but the
// declarations themselves remain fingerprinted as stable Core API.

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

#include <BaseTsd.h>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

using socklen_t = int;
using ssize_t = SSIZE_T;

namespace gamenet::net {

using SocketFd = SOCKET;
using sa_family_t = ADDRESS_FAMILY;

inline constexpr SocketFd kInvalidSocket = INVALID_SOCKET;

}  // namespace gamenet::net

#else

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace gamenet::net {

using SocketFd = int;
using ::sa_family_t;

inline constexpr SocketFd kInvalidSocket = -1;

}  // namespace gamenet::net

#endif
