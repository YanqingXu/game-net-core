#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cassert>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {

gamenet::net::SocketFd connectTo(const gamenet::net::InetAddress& serverAddr) {
    gamenet::net::SocketFd fd = gamenet::net::sockets::createNonblockingOrDie(serverAddr.family());
    const int rc = gamenet::net::sockets::connect(fd, serverAddr.getSockAddr(), serverAddr.getSockAddrLen());
    if (rc < 0) {
        const int error = gamenet::net::sockets::lastError();
        assert(gamenet::net::sockets::isInProgress(error) || gamenet::net::sockets::isWouldBlock(error));
    }
    return fd;
}

gamenet::net::SocketFd acceptEventually(gamenet::net::Socket& listener, gamenet::net::InetAddress* peerAddr) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        gamenet::net::SocketFd accepted = listener.accept(peerAddr);
        if (gamenet::net::sockets::isValid(accepted)) {
            return accepted;
        }

        const int error = gamenet::net::sockets::lastError();
        assert(gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error));
        std::this_thread::sleep_for(10ms);
    }
    assert(false && "timed out waiting for socket accept");
    return gamenet::net::kInvalidSocket;
}

}  // namespace

int main() {
    gamenet::net::Socket listener(gamenet::net::sockets::createNonblockingOrDie(AF_INET));
    listener.setReuseAddr(true);
    listener.bindAddress(gamenet::net::InetAddress(0, true));
    listener.listen();

    const gamenet::net::InetAddress listenAddr(gamenet::net::sockets::getLocalAddr(listener.fd()));
    assert(listenAddr.isIpv4());
    assert(listenAddr.port() != 0);

    gamenet::net::SocketFd client = connectTo(listenAddr);

    gamenet::net::InetAddress peerAddr;
    gamenet::net::SocketFd accepted = acceptEventually(listener, &peerAddr);

    assert(gamenet::net::sockets::isValid(accepted));
    assert(peerAddr.isIpv4());
    assert(peerAddr.toIp() == "127.0.0.1");
    assert(peerAddr.port() != 0);

    gamenet::net::sockets::close(accepted);
    gamenet::net::sockets::close(client);

    return 0;
}
