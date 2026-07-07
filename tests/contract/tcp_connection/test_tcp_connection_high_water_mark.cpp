#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void setNonBlockingForTest(gamenet::net::SocketFd sockfd) {
#ifndef _WIN32
    const int flags = ::fcntl(sockfd, F_GETFL, 0);
    GAMENET_TEST_ASSERT(flags >= 0);
    GAMENET_TEST_ASSERT(::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == 0);
#else
    (void)sockfd;
#endif
}

void setSmallSendBuffer(gamenet::net::SocketFd sockfd) {
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

struct ConnectedPair {
    gamenet::net::SocketFd connectionFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd peerFd{gamenet::net::kInvalidSocket};

    ConnectedPair() {
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
        setSmallSendBuffer(connectionFd);
    }

    ~ConnectedPair() {
        if (gamenet::net::sockets::isValid(peerFd)) {
            gamenet::net::sockets::close(peerFd);
        }
    }
};

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    ConnectedPair pair;

    const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
    const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

    auto connection = std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        "high-water-mark-fires-on-owner-loop",
        pair.connectionFd,
        localAddr,
        peerAddr);

    constexpr std::size_t highWaterMark = 1024;
    int highWaterCallbackCount = 0;
    int closeCallbackCount = 0;
    std::size_t highWaterBytes = 0;
    bool sendReturned = false;
    bool highWaterBeforeSendReturned = false;

    connection->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& conn, std::size_t bufferedBytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++highWaterCallbackCount;
            highWaterBytes = bufferedBytes;
            if (!sendReturned) {
                highWaterBeforeSendReturned = true;
            }
            conn->forceClose();
        },
        highWaterMark);

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    const std::string payload(4 * 1024 * 1024, 'h');
    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->send(payload);
        sendReturned = true;
        GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for high-water callback");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(sendReturned);
    GAMENET_TEST_ASSERT(!highWaterBeforeSendReturned);
    GAMENET_TEST_ASSERT(highWaterCallbackCount == 1);
    GAMENET_TEST_ASSERT(highWaterBytes >= highWaterMark);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
