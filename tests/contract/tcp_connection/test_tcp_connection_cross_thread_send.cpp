#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerId.h"

#include "support/TestAssert.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

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
        "cross-thread-send-marshals-to-owner-loop",
        pair.connectionFd,
        localAddr,
        peerAddr);

    const std::string payload = "cross-thread-send-payload";

    std::size_t peerReadBytes = 0;
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int writeCompleteCallbackCount = 0;
    int closeCallbackCount = 0;
    bool sendIssued = false;
    bool closeIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenContractObserved = [&] {
        if (!closeIssued && peerReadBytes == payload.size() && writeCompleteCallbackCount == 1) {
            closeIssued = true;
            loop.cancel(drainTimer);
            connection->forceClose();
        }
    };

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++connectedCallbackCount;
            return;
        }
        ++disconnectedCallbackCount;
    });

    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbackCount;
        closeWhenContractObserved();
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    drainTimer = loop.runEvery(std::chrono::milliseconds(1), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        char buffer[64];
        while (peerReadBytes < payload.size()) {
            const ssize_t n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                peerReadBytes += static_cast<std::size_t>(n);
                GAMENET_TEST_ASSERT(peerReadBytes <= payload.size());
                continue;
            }
            if (n == 0) {
                GAMENET_TEST_ASSERT(false && "peer saw EOF before cross-thread send payload");
            }

            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error)) {
                closeWhenContractObserved();
                return;
            }

            GAMENET_TEST_ASSERT(false && "unexpected peer read error while draining cross-thread send payload");
        }
        closeWhenContractObserved();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());

        // cross-thread-send-marshals-to-owner-loop: send() from a non-owner
        // thread must copy the payload, marshal through EventLoop, and write
        // from the owner loop without executing user callbacks off-thread.
        std::thread sendThread([conn, &payload] {
            conn->send(payload);
        });
        sendThread.join();
        sendIssued = true;
    });

    loop.runAfter(std::chrono::seconds(2), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for cross-thread send delivery");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(sendIssued);
    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1);
    GAMENET_TEST_ASSERT(peerReadBytes == payload.size());
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
