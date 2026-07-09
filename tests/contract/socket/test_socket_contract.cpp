#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {

gamenet::net::SocketFd acceptEventually(gamenet::net::Socket& listener, gamenet::net::InetAddress* peerAddr) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        gamenet::net::SocketFd accepted = listener.accept(peerAddr);
        if (gamenet::net::sockets::isValid(accepted)) {
            return accepted;
        }

        const int error = gamenet::net::sockets::lastError();
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error));
        std::this_thread::sleep_for(10ms);
    }
    GAMENET_TEST_ASSERT(false && "timed out waiting for socket accept");
    return gamenet::net::kInvalidSocket;
}

}  // namespace

int main() {
    gamenet::net::Socket listener(gamenet::net::sockets::createNonblockingOrDie(AF_INET));
    listener.setReuseAddr(true);
    listener.bindAddress(gamenet::net::InetAddress(0, true));
    listener.listen();

    const gamenet::net::InetAddress listenAddr(gamenet::net::sockets::getLocalAddr(listener.fd()));
    GAMENET_TEST_ASSERT(listenAddr.isIpv4());
    GAMENET_TEST_ASSERT(listenAddr.port() != 0);

    gamenet::net::SocketFd client = gamenet::test::connectTestClient(listenAddr);

    gamenet::net::InetAddress peerAddr;
    gamenet::net::SocketFd accepted = acceptEventually(listener, &peerAddr);

    GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(accepted));
    GAMENET_TEST_ASSERT(peerAddr.isIpv4());
    GAMENET_TEST_ASSERT(peerAddr.toIp() == "127.0.0.1");
    GAMENET_TEST_ASSERT(peerAddr.port() != 0);

    gamenet::test::closeTestSocket(accepted);
    gamenet::test::closeTestSocket(client);

    return 0;
}
