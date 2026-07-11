#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <array>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

int main() {
    {
        gamenet::net::EventLoop loop;
        gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

        constexpr std::size_t kClientCount = 3;
        std::size_t accepted = 0;

        acceptor.setNewConnectionCallback(
            [&](gamenet::net::SocketFd sockfd, const gamenet::net::InetAddress& peerAddr) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(sockfd));
                GAMENET_TEST_ASSERT(!peerAddr.toIp().empty());

                ++accepted;
                gamenet::test::closeTestSocket(sockfd);

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
                fd = gamenet::test::connectTestClient(listenAddr);
            }
            std::this_thread::sleep_for(80ms);
            for (auto fd : fds) {
                gamenet::test::closeTestSocket(fd);
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
    }

    {
        gamenet::net::EventLoop loop;
        auto acceptor = std::make_unique<gamenet::net::Acceptor>(
            &loop, gamenet::net::InetAddress(0, true), true);
        acceptor->listen();
        loop.runAfter(0s, [&] {
            acceptor->stop();
            acceptor.reset();
        });
        loop.runAfter(20ms, [&] { loop.quit(); });
        loop.loop();
        GAMENET_TEST_ASSERT(!acceptor);
    }

    return 0;
}
