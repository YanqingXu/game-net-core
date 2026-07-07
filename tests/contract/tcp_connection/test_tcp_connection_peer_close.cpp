#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <chrono>
#include <memory>

#ifdef _WIN32
#include <array>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

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
    }

    ~ConnectedPair() {
        if (gamenet::net::sockets::isValid(peerFd)) {
            gamenet::net::sockets::close(peerFd);
        }
    }

    void closePeer() {
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(peerFd));
        gamenet::net::sockets::close(peerFd);
        peerFd = gamenet::net::kInvalidSocket;
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
        "peer-close-converges-once",
        pair.connectionFd,
        localAddr,
        peerAddr);

    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int closeCallbackCount = 0;

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
        loop.quit();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());
        pair.closePeer();
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for peer close teardown");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
