#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace {

gamenet::net::SocketFd connectTo(const gamenet::net::InetAddress& serverAddr) {
    gamenet::net::SocketFd fd = gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    const int rc = gamenet::net::sockets::connect(fd, serverAddr.getSockAddr(), serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isInProgress(error) || gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

void setResetOnClose(gamenet::net::SocketFd fd) {
    linger resetOnClose{};
    resetOnClose.l_onoff = 1;
    resetOnClose.l_linger = 0;

#ifdef _WIN32
    const int rc = ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_LINGER,
        reinterpret_cast<const char*>(&resetOnClose),
        static_cast<socklen_t>(sizeof(resetOnClose)));
#else
    const int rc = ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_LINGER,
        &resetOnClose,
        static_cast<socklen_t>(sizeof(resetOnClose)));
#endif
    GAMENET_TEST_ASSERT(rc == 0);
}

struct LoopbackTcpPair {
    gamenet::net::SocketFd connectionFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd peerFd{gamenet::net::kInvalidSocket};

    LoopbackTcpPair() {
        gamenet::net::SocketFd listener = gamenet::net::sockets::createNonblockingOrDie(AF_INET);

        gamenet::net::InetAddress bindAddr(0, true);
        sockaddr_storage bindStorage{};
        std::memcpy(&bindStorage, bindAddr.getSockAddr(), bindAddr.getSockAddrLen());
        gamenet::net::sockets::bindOrDie(listener, bindStorage);
        gamenet::net::sockets::listenOrDie(listener);

        const gamenet::net::InetAddress listenAddr(gamenet::net::sockets::getLocalAddr(listener));
        peerFd = connectTo(listenAddr);

        for (int attempt = 0; attempt < 1000; ++attempt) {
            sockaddr_storage peerAddr{};
            connectionFd = gamenet::net::sockets::accept(listener, &peerAddr);
            if (gamenet::net::sockets::isValid(connectionFd)) {
                break;
            }

            const int error = gamenet::net::sockets::lastError();
            GAMENET_TEST_ASSERT(gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        gamenet::net::sockets::close(listener);
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(connectionFd));
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(peerFd));
    }

    ~LoopbackTcpPair() {
        if (gamenet::net::sockets::isValid(peerFd)) {
            gamenet::net::sockets::close(peerFd);
        }
    }

    void resetPeer() {
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(peerFd));
        setResetOnClose(peerFd);
        gamenet::net::sockets::close(peerFd);
        peerFd = gamenet::net::kInvalidSocket;
    }
};

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    LoopbackTcpPair pair;

    const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
    const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

    auto connection = std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        "peer-reset-error-converges-once",
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
        conn->forceClose();
        conn->connectDestroyed();
        loop.quit();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());
        pair.resetPeer();
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for peer reset teardown");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    return 0;
}
