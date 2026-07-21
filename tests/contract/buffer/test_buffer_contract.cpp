#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/SocketTypes.h"

#include "support/TestAssert.h"
#include "support/SocketPair.h"
#include <string>

int main() {
    {
        gamenet::net::Buffer buffer;
        buffer.append("abcdef", 6);
        GAMENET_TEST_ASSERT(buffer.retrieveAsString(2) == "ab");

        const std::string payload(gamenet::net::Buffer::kInitialSize, 'x');
        buffer.append(payload);

        GAMENET_TEST_ASSERT(buffer.readableBytes() == 4 + payload.size());
        GAMENET_TEST_ASSERT(buffer.retrieveAsString(4) == "cdef");
        GAMENET_TEST_ASSERT(buffer.retrieveAllAsString() == payload);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 0);
        GAMENET_TEST_ASSERT(buffer.prependableBytes() == gamenet::net::Buffer::kCheapPrepend);
    }

    {
        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(gamenet::net::kInvalidSocket, &savedErrno);
        GAMENET_TEST_ASSERT(n < 0);
        GAMENET_TEST_ASSERT(savedErrno != 0);
    }

    {
        gamenet::test::ConnectedSocketPair pair;
        const std::string payload(128, 'i');
        GAMENET_TEST_ASSERT(
            gamenet::net::sockets::write(pair.peerFd, payload.data(), payload.size()) ==
            static_cast<ssize_t>(payload.size()));

        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(pair.connectionFd, &savedErrno, 16);
        GAMENET_TEST_ASSERT(n == 16);
        GAMENET_TEST_ASSERT(buffer.readableBytes() == 16);
        GAMENET_TEST_ASSERT(buffer.retrieveAllAsString() == payload.substr(0, 16));

        gamenet::net::sockets::close(pair.connectionFd);
        pair.connectionFd = gamenet::net::kInvalidSocket;
    }

    return 0;
}
