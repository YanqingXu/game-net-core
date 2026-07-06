#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include <array>
#include "support/TestAssert.h"
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

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

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

    constexpr std::size_t kClientCount = 3;
    std::size_t accepted = 0;

    acceptor.setNewConnectionCallback([&](gamenet::net::SocketFd sockfd, const gamenet::net::InetAddress& peerAddr) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(sockfd));
        GAMENET_TEST_ASSERT(!peerAddr.toIp().empty());

        ++accepted;
        gamenet::net::sockets::close(sockfd);

        if (accepted == kClientCount) {
            acceptor.stop();
            loop.quit();
        }
    });

    GAMENET_TEST_ASSERT(!acceptor.listening());
    acceptor.listen();
    GAMENET_TEST_ASSERT(acceptor.listening());

    const gamenet::net::InetAddress listenAddr = acceptor.listenAddress();
    std::thread clients([listenAddr] {
        std::this_thread::sleep_for(30ms);
        std::array<gamenet::net::SocketFd, kClientCount> fds{};
        for (auto& fd : fds) {
            fd = connectTo(listenAddr);
        }
        std::this_thread::sleep_for(80ms);
        for (auto fd : fds) {
            gamenet::net::sockets::close(fd);
        }
    });

    loop.runAfter(1s, [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for accepted connections");
        loop.quit();
    });
    loop.loop();
    clients.join();

    GAMENET_TEST_ASSERT(accepted == kClientCount);
    GAMENET_TEST_ASSERT(!acceptor.listening());

    return 0;
}
