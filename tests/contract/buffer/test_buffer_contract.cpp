#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/SocketTypes.h"

#include <cassert>
#include <string>

int main() {
    {
        gamenet::net::Buffer buffer;
        buffer.append("abcdef", 6);
        assert(buffer.retrieveAsString(2) == "ab");

        const std::string payload(gamenet::net::Buffer::kInitialSize, 'x');
        buffer.append(payload);

        assert(buffer.readableBytes() == 4 + payload.size());
        assert(buffer.retrieveAsString(4) == "cdef");
        assert(buffer.retrieveAllAsString() == payload);
        assert(buffer.readableBytes() == 0);
        assert(buffer.prependableBytes() == gamenet::net::Buffer::kCheapPrepend);
    }

    {
        gamenet::net::Buffer buffer;
        int savedErrno = 0;
        const ssize_t n = buffer.readFd(gamenet::net::kInvalidSocket, &savedErrno);
        assert(n < 0);
        assert(savedErrno != 0);
    }

    return 0;
}
