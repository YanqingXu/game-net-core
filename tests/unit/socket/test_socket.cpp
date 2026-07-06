#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cassert>

int main() {
    {
        gamenet::net::Socket socket(
            gamenet::net::sockets::createNonblockingOrDie(AF_INET));
        assert(gamenet::net::sockets::isValid(socket.fd()));

        socket.setReuseAddr(true);
        socket.setReusePort(true);
        socket.setKeepAlive(true);
        socket.setTcpNoDelay(true);

        const gamenet::net::SocketFd released = socket.releaseFd();
        assert(gamenet::net::sockets::isValid(released));
        assert(socket.fd() == gamenet::net::kInvalidSocket);
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
