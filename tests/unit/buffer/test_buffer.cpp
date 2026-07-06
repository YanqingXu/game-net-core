#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cassert>
#include <cerrno>
#include <string>

#ifdef _WIN32
#include <array>
#else
#include <unistd.h>
#endif

namespace {

struct TestPipe {
    gamenet::net::SocketFd readFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd writeFd{gamenet::net::kInvalidSocket};

    TestPipe() {
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
        assert(::pipe(fds) == 0);
        readFd = fds[0];
        writeFd = fds[1];
#endif
    }

    ~TestPipe() {
        gamenet::net::sockets::close(readFd);
        gamenet::net::sockets::close(writeFd);
    }
};

}  // namespace

int main() {
    {
        gamenet::net::Buffer buffer;
        buffer.append("hello", 5);
        assert(buffer.readableBytes() == 5);
        assert(buffer.retrieveAsString(2) == "he");
        assert(buffer.readableBytes() == 3);
        assert(buffer.retrieveAllAsString() == "llo");
        assert(buffer.readableBytes() == 0);
    }

    {
        TestPipe pipe;
        const std::string payload(gamenet::net::Buffer::kInitialSize + 512, 'x');
        const ssize_t written = gamenet::net::sockets::write(pipe.writeFd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));

        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(pipe.readFd, &savedErrno);
        assert(n == static_cast<ssize_t>(payload.size()));
        assert(buffer.readableBytes() == payload.size());
        assert(buffer.retrieveAllAsString() == payload);
    }

    {
        TestPipe pipe;
        gamenet::net::Buffer buffer;
        const std::string payload = "write through buffer";
        buffer.append(payload);

        int savedErrno = 0;
        const ssize_t n = buffer.writeFd(pipe.writeFd, &savedErrno);
        assert(n == static_cast<ssize_t>(payload.size()));

        char readBuf[64] = {};
        const ssize_t r = gamenet::net::sockets::read(pipe.readFd, readBuf, sizeof(readBuf));
        assert(r == static_cast<ssize_t>(payload.size()));
        assert(std::string(readBuf, static_cast<std::size_t>(r)) == payload);
    }

    {
        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        assert(buffer.writeFd(gamenet::net::kInvalidSocket, &savedErrno) == -1);
        assert(savedErrno != 0);
    }

    return 0;
}
