#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32

void verifyPeerCloseWriteDoesNotRaiseSigpipe() {
    const pid_t child = ::fork();
    GAMENET_TEST_ASSERT(child >= 0);

    if (child == 0) {
        // Prove the socket helper is safe even when the embedding process keeps
        // the operating-system default (terminating) SIGPIPE disposition.
        std::signal(SIGPIPE, SIG_DFL);

        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            _exit(2);
        }

        ::close(sockets[1]);
        errno = 0;
        const char byte = 'x';
        const auto written = gamenet::net::sockets::write(sockets[0], &byte, sizeof(byte));
        const int writeError = errno;
        ::close(sockets[0]);

        _exit(written == -1 && writeError == EPIPE ? 0 : 3);
    }

    int status = 0;
    GAMENET_TEST_ASSERT(::waitpid(child, &status, 0) == child);
    GAMENET_TEST_ASSERT(WIFEXITED(status));
    GAMENET_TEST_ASSERT(WEXITSTATUS(status) == 0);
}

#endif

}  // namespace

int main() {
#ifndef _WIN32
    verifyPeerCloseWriteDoesNotRaiseSigpipe();
#endif

    {
        const auto unsupportedFamily = static_cast<gamenet::net::sa_family_t>(-1);
        const auto unsupported = gamenet::net::sockets::createNonblocking(unsupportedFamily);
        GAMENET_TEST_ASSERT(!gamenet::net::sockets::isValid(unsupported));
        GAMENET_TEST_ASSERT(gamenet::net::sockets::lastError() != 0);

        const gamenet::net::InetAddress local(0, true);
        sockaddr_storage storage{};
        std::memcpy(&storage, local.getSockAddr(), local.getSockAddrLen());
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::bind(gamenet::net::kInvalidSocket, storage) < 0);
        GAMENET_TEST_ASSERT(gamenet::net::sockets::lastError() != 0);

        gamenet::net::Socket invalid(gamenet::net::kInvalidSocket);
        int error = 0;
        GAMENET_TEST_ASSERT(!invalid.tryBindAddress(local, &error));
        GAMENET_TEST_ASSERT(error != 0);
        error = 0;
        GAMENET_TEST_ASSERT(!invalid.tryListen(&error));
        GAMENET_TEST_ASSERT(error != 0);
        sockaddr_storage queriedAddress{};
        GAMENET_TEST_ASSERT(!gamenet::net::sockets::tryGetLocalAddr(
            gamenet::net::kInvalidSocket,
            &queriedAddress));
        GAMENET_TEST_ASSERT(!gamenet::net::sockets::tryGetPeerAddr(
            gamenet::net::kInvalidSocket,
            &queriedAddress));
    }

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
