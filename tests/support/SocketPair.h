#pragma once

#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace gamenet::test {

enum class SocketPairMode {
    Default,
    SmallSendBuffer,
};

inline void setNonBlockingForTest(gamenet::net::SocketFd sockfd) {
#ifndef _WIN32
    const int flags = ::fcntl(sockfd, F_GETFL, 0);
    GAMENET_TEST_ASSERT(flags >= 0);
    GAMENET_TEST_ASSERT(::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == 0);
#else
    (void)sockfd;
#endif
}

inline void setSmallSendBufferForTest(gamenet::net::SocketFd sockfd) {
    int bufferSize = 4096;
#ifdef _WIN32
    const int rc = ::setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&bufferSize),
        static_cast<socklen_t>(sizeof(bufferSize)));
#else
    const int rc = ::setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_SNDBUF,
        &bufferSize,
        static_cast<socklen_t>(sizeof(bufferSize)));
#endif
    GAMENET_TEST_ASSERT(rc == 0);
}

struct ConnectedSocketPair {
    gamenet::net::SocketFd connectionFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd peerFd{gamenet::net::kInvalidSocket};

    explicit ConnectedSocketPair(SocketPairMode mode = SocketPairMode::Default) {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        connectionFd = fds[0];
        peerFd = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        connectionFd = fds[0];
        peerFd = fds[1];
#endif
        setNonBlockingForTest(connectionFd);
        setNonBlockingForTest(peerFd);
        if (mode == SocketPairMode::SmallSendBuffer) {
            setSmallSendBufferForTest(connectionFd);
        }
    }

    ConnectedSocketPair(const ConnectedSocketPair&) = delete;
    ConnectedSocketPair& operator=(const ConnectedSocketPair&) = delete;

    void closePeer() {
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(peerFd));
        gamenet::net::sockets::close(peerFd);
        peerFd = gamenet::net::kInvalidSocket;
    }

    ~ConnectedSocketPair() {
        if (gamenet::net::sockets::isValid(peerFd)) {
            gamenet::net::sockets::close(peerFd);
        }
    }
};

}  // namespace gamenet::test
