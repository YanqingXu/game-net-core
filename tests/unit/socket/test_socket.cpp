#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"

int main() {
    {
        gamenet::net::Socket socket(
            gamenet::net::sockets::createNonblockingOrDie(AF_INET));
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(socket.fd()));

        socket.setReuseAddr(true);
        socket.setReusePort(true);
        socket.setKeepAlive(true);
        socket.setTcpNoDelay(true);

        const gamenet::net::SocketFd released = socket.releaseFd();
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(released));
        GAMENET_TEST_ASSERT(socket.fd() == gamenet::net::kInvalidSocket);
        gamenet::net::sockets::close(released);
    }

    {
        gamenet::net::Socket socket(
            gamenet::net::sockets::createNonblockingOrDie(AF_INET));
        socket.bindAddress(gamenet::net::InetAddress(0, true));
        socket.listen();
    }

    return 0;
}
