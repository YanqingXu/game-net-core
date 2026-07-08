#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <array>
#else
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
    constexpr int iterationCount = 8;

    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        gamenet::net::EventLoop loop;
        ConnectedPair pair;

        const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
        const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

        std::shared_ptr<gamenet::net::TcpConnection> connection = std::make_shared<gamenet::net::TcpConnection>(
            &loop,
            "force-close-pending-write-mixed-timing-soak-" + std::to_string(iteration),
            pair.connectionFd,
            localAddr,
            peerAddr);

        int connectedCallbackCount = 0;
        int disconnectedCallbackCount = 0;
        int closeCallbackCount = 0;
        std::atomic<bool> forceCloseIssued{false};
        std::atomic<bool> ownerForceCloseIssued{false};
        std::atomic<bool> nonOwnerForceCloseIssued{false};
        std::thread closer;

        connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (conn->connected()) {
                ++connectedCallbackCount;
                return;
            }
            ++disconnectedCallbackCount;
        });

        connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++closeCallbackCount;
            conn->connectDestroyed();
            connection.reset();
            loop.runAfter(std::chrono::milliseconds(50), [&] { loop.quit(); });
        });

        loop.runAfter(std::chrono::milliseconds(0), [&] {
            auto conn = connection;
            conn->connectEstablished();
            GAMENET_TEST_ASSERT(conn->connected());

            const std::string payload(8 * 1024 * 1024, 'm');
            conn->send(payload);

            // force-close-pending-write-mixed-timing-soak: immediate and
            // delayed owner/non-owner forceClose() calls must converge on one
            // owner-loop cancel/close path while a large write may be pending.
            const int mode = iteration % 4;
            if (mode == 0) {
                forceCloseIssued = true;
                ownerForceCloseIssued = true;
                conn->forceClose();
            } else if (mode == 1) {
                closer = std::thread([conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued] {
                    GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                    forceCloseIssued = true;
                    nonOwnerForceCloseIssued = true;
                    conn->forceClose();
                });
            } else if (mode == 2) {
                loop.runAfter(
                    std::chrono::milliseconds(5 + iteration),
                    [conn, &loop, &forceCloseIssued, &ownerForceCloseIssued] {
                        GAMENET_TEST_ASSERT(loop.isInLoopThread());
                        forceCloseIssued = true;
                        ownerForceCloseIssued = true;
                        conn->forceClose();
                    });
            } else {
                closer = std::thread([conn, &loop, &forceCloseIssued, &nonOwnerForceCloseIssued, iteration] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5 + iteration));
                    GAMENET_TEST_ASSERT(!loop.isInLoopThread());
                    forceCloseIssued = true;
                    nonOwnerForceCloseIssued = true;
                    conn->forceClose();
                });
            }
        });

        loop.runAfter(std::chrono::seconds(2), [&] {
            GAMENET_TEST_ASSERT(false && "timed out waiting for mixed-timing pending-write forceClose teardown");
            loop.quit();
        });

        loop.loop();

        if (closer.joinable()) {
            closer.join();
        }

        GAMENET_TEST_ASSERT(forceCloseIssued.load());
        GAMENET_TEST_ASSERT(ownerForceCloseIssued.load() || nonOwnerForceCloseIssued.load());
        GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
        GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
        GAMENET_TEST_ASSERT(closeCallbackCount == 1);
        GAMENET_TEST_ASSERT(!connection);
    }

    return 0;
}
