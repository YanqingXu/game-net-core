#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <limits>
#include <stdexcept>
#include <thread>

namespace {

int sendBufferSize(gamenet::net::SocketFd fd) {
    int value = 0;
    socklen_t length = static_cast<socklen_t>(sizeof(value));
#ifdef _WIN32
    const int result = ::getsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<char*>(&value),
        &length);
#else
    const int result = ::getsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDBUF,
        &value,
        &length);
#endif
    GAMENET_TEST_ASSERT(result == 0);
    GAMENET_TEST_ASSERT(length == sizeof(value));
    return value;
}

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection =
        gamenet::test::makeTcpConnection(loop, pair, "socket-options#1");

    constexpr std::size_t requestedBytes = 16U * 1024U;
    connection->setSendBufferSize(requestedBytes);
    const int effectiveBytes = sendBufferSize(pair.connectionFd);
    GAMENET_TEST_ASSERT(effectiveBytes >= static_cast<int>(requestedBytes));
    GAMENET_TEST_ASSERT(
        effectiveBytes <= static_cast<int>(requestedBytes * 2U));

    bool zeroRejected = false;
    try {
        connection->setSendBufferSize(0);
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    GAMENET_TEST_ASSERT(zeroRejected);

    bool oversizedRejected = false;
    try {
        connection->setSendBufferSize(
            (std::numeric_limits<std::size_t>::max)());
    } catch (const std::invalid_argument&) {
        oversizedRejected = true;
    }
    GAMENET_TEST_ASSERT(oversizedRejected);

    bool wrongThreadRejected = false;
    std::thread wrongThread([&] {
        try {
            connection->setSendBufferSize(requestedBytes);
        } catch (const std::runtime_error&) {
            wrongThreadRejected = true;
        }
    });
    wrongThread.join();
    GAMENET_TEST_ASSERT(wrongThreadRejected);

    return 0;
}
