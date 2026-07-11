#pragma once

#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

#include <vector>

namespace gamenet::test {

inline void setReceiveBufferForTest(gamenet::net::SocketFd fd, int bytes) {
    GAMENET_TEST_ASSERT(bytes > 0);
#ifdef _WIN32
    const int rc = ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<const char*>(&bytes),
        static_cast<int>(sizeof(bytes)));
    GAMENET_TEST_ASSERT(rc != SOCKET_ERROR);
#else
    const int rc = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    GAMENET_TEST_ASSERT(rc == 0);
#endif
}

inline gamenet::net::SocketFd connectTestClient(const gamenet::net::InetAddress& serverAddr) {
    gamenet::net::SocketFd fd = gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    const int rc = gamenet::net::sockets::connect(
        fd,
        serverAddr.getSockAddr(),
        serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::isInProgress(error) ||
            gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

inline gamenet::net::SocketFd connectTestClientWithReceiveBuffer(
    const gamenet::net::InetAddress& serverAddr,
    int receiveBufferBytes) {
    gamenet::net::SocketFd fd =
        gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    setReceiveBufferForTest(fd, receiveBufferBytes);
    const int rc = gamenet::net::sockets::connect(
        fd,
        serverAddr.getSockAddr(),
        serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::isInProgress(error) ||
            gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

inline void closeTestSocket(gamenet::net::SocketFd fd) {
    gamenet::net::sockets::close(fd);
}

inline void closeTestSockets(const std::vector<gamenet::net::SocketFd>& fds) {
    for (const auto fd : fds) {
        closeTestSocket(fd);
    }
}

}  // namespace gamenet::test
