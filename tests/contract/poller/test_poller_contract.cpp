#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <chrono>
#include <string_view>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

struct ReadablePair {
    gamenet::net::SocketFd readFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd writeFd{gamenet::net::kInvalidSocket};

    ReadablePair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        readFd = fds[0];
        writeFd = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        readFd = fds[0];
        writeFd = fds[1];
#endif
    }

    ~ReadablePair() {
        gamenet::net::sockets::close(readFd);
        gamenet::net::sockets::close(writeFd);
    }
};

void writePayload(gamenet::net::SocketFd fd, std::string_view payload) {
    const ssize_t written = gamenet::net::sockets::write(fd, payload.data(), payload.size());
    GAMENET_TEST_ASSERT(written == static_cast<ssize_t>(payload.size()));
}

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    ReadablePair pair;
    gamenet::net::Channel channel(&loop, pair.readFd);

    int readCount = 0;
    bool removed = false;

    channel.setReadCallback([&](gamenet::base::Timestamp) {
        ++readCount;

        char buffer[16] = {};
        const ssize_t n = gamenet::net::sockets::read(pair.readFd, buffer, sizeof(buffer));
        GAMENET_TEST_ASSERT(n == 4);
        GAMENET_TEST_ASSERT(std::string_view(buffer, static_cast<std::size_t>(n)) == "ping");

        channel.disableAll();
        channel.remove();
        GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));
        removed = true;
        writePayload(pair.writeFd, "pong");
        loop.runAfter(20ms, [&] { loop.quit(); });
    });

    channel.enableReading();
    GAMENET_TEST_ASSERT(loop.hasChannel(&channel));

    writePayload(pair.writeFd, "ping");
    loop.loop();

    GAMENET_TEST_ASSERT(readCount == 1);
    GAMENET_TEST_ASSERT(removed);
    GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));

    return 0;
}
