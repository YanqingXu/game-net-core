#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "support/TestAssert.h"
#include <chrono>

using namespace std::chrono_literals;

int main() {
    {
        gamenet::net::EventLoop loop;
        gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

        GAMENET_TEST_ASSERT(!acceptor.listening());

        loop.runAfter(0s, [&] {
            acceptor.listen();
            GAMENET_TEST_ASSERT(acceptor.listening());
            acceptor.stop();
            GAMENET_TEST_ASSERT(!acceptor.listening());
            loop.quit();
        });

        loop.loop();
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

        loop.runAfter(0s, [&] {
            GAMENET_TEST_ASSERT(!acceptor.listening());
            acceptor.stop();
            GAMENET_TEST_ASSERT(!acceptor.listening());
            loop.quit();
        });

        loop.loop();
    }

    return 0;
}
