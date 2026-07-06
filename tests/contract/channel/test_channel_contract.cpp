#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/TestAssert.h"
#include <memory>
#include <stdexcept>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

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

}  // namespace

int main() {
    {
        gamenet::net::EventLoop loop;
        ReadablePair pair;
        gamenet::net::Channel channel(&loop, pair.readFd);

        GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));

        channel.enableReading();
        GAMENET_TEST_ASSERT(loop.hasChannel(&channel));
        GAMENET_TEST_ASSERT(channel.isReading());
        GAMENET_TEST_ASSERT(!channel.isWriting());

        channel.enableWriting();
        GAMENET_TEST_ASSERT(channel.isReading());
        GAMENET_TEST_ASSERT(channel.isWriting());

        bool removeRejected = false;
        try {
            channel.remove();
        } catch (const std::runtime_error&) {
            removeRejected = true;
        }
        GAMENET_TEST_ASSERT(removeRejected);
        GAMENET_TEST_ASSERT(loop.hasChannel(&channel));

        channel.disableAll();
        GAMENET_TEST_ASSERT(channel.isNoneEvent());
        channel.remove();
        GAMENET_TEST_ASSERT(!loop.hasChannel(&channel));
    }

    {
        gamenet::net::EventLoop loop;
        constexpr gamenet::net::SocketFd fakeFd = static_cast<gamenet::net::SocketFd>(0);
        gamenet::net::Channel channel(&loop, fakeFd);

        bool readFired = false;
        bool writeFired = false;
        bool errorFired = false;
        bool closeFired = false;

        channel.setReadCallback([&](gamenet::base::Timestamp) { readFired = true; });
        channel.setWriteCallback([&] { writeFired = true; });
        channel.setErrorCallback([&] { errorFired = true; });
        channel.setCloseCallback([&] { closeFired = true; });

        channel.setRevents(gamenet::net::Channel::kReadEvent);
        channel.handleEvent(gamenet::base::now());
        GAMENET_TEST_ASSERT(readFired);
        GAMENET_TEST_ASSERT(!writeFired);
        GAMENET_TEST_ASSERT(!errorFired);
        GAMENET_TEST_ASSERT(!closeFired);

        channel.setRevents(gamenet::net::Channel::kWriteEvent);
        channel.handleEvent(gamenet::base::now());
        GAMENET_TEST_ASSERT(writeFired);

        channel.setRevents(gamenet::net::Channel::kErrorEvent);
        channel.handleEvent(gamenet::base::now());
        GAMENET_TEST_ASSERT(errorFired);

        channel.setRevents(gamenet::net::Channel::kCloseEvent);
        channel.handleEvent(gamenet::base::now());
        GAMENET_TEST_ASSERT(closeFired);
    }

    {
        gamenet::net::EventLoop loop;
        constexpr gamenet::net::SocketFd fakeFd = static_cast<gamenet::net::SocketFd>(0);
        gamenet::net::Channel channel(&loop, fakeFd);
        bool readFired = false;
        channel.setReadCallback([&](gamenet::base::Timestamp) { readFired = true; });

        {
            auto owner = std::make_shared<int>(42);
            channel.tie(owner);
            channel.setRevents(gamenet::net::Channel::kReadEvent);
            channel.handleEvent(gamenet::base::now());
            GAMENET_TEST_ASSERT(readFired);
        }

        readFired = false;
        channel.setRevents(gamenet::net::Channel::kReadEvent);
        channel.handleEvent(gamenet::base::now());
        GAMENET_TEST_ASSERT(!readFired);
    }

    return 0;
}
