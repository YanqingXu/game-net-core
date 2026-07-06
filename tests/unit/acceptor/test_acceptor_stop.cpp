#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include <cassert>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    {
        gamenet::net::EventLoop loop;
        gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

        assert(!acceptor.listening());

        loop.runAfter(0s, [&] {
            acceptor.listen();
            assert(acceptor.listening());
            acceptor.stop();
            assert(!acceptor.listening());
            loop.quit();
        });

        loop.loop();
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::net::Acceptor acceptor(&loop, gamenet::net::InetAddress(0, true), true);

        loop.runAfter(0s, [&] {
            assert(!acceptor.listening());
            acceptor.stop();
            assert(!acceptor.listening());
            loop.quit();
        });

        loop.loop();
    }

    return 0;
}
