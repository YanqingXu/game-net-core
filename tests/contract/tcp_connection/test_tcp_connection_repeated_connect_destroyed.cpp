#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"

#include "support/LoopTest.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionCallbacks.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <chrono>
#include <memory>
#include <string_view>

using namespace std::chrono_literals;

int main() {
    gamenet::net::EventLoop loop;
    auto pair = std::make_unique<gamenet::test::ConnectedSocketPair>();
    auto connection = gamenet::test::makeTcpConnection(
        loop,
        *pair,
        "repeated-connect-destroyed-removes-registration");

    gamenet::test::TcpConnectionCallbackCounts callbacks;
    gamenet::test::setCountingConnectionCallback(connection, loop, callbacks);

    int messageCallbackCount = 0;
    bool postDestroyPeerWriteAttempted = false;
    bool outerConnectionReleased = false;

    connection->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr&, gamenet::net::Buffer* buffer) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++messageCallbackCount;
            buffer->retrieveAll();
        });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++callbacks.closed;

        // repeated-connect-destroyed-removes-registration: repeated
        // connectDestroyed() calls after forceClose must not double-remove the
        // Channel or leave a stale readable registration behind.
        conn->connectDestroyed();
        conn->connectDestroyed();

        constexpr std::string_view payload = "x";
        const ssize_t n = gamenet::net::sockets::write(pair->peerFd, payload.data(), payload.size());
        if (n < 0) {
            const int error = gamenet::net::sockets::lastError();
            GAMENET_TEST_ASSERT(
                gamenet::net::sockets::isWouldBlock(error) ||
                gamenet::net::sockets::isInterrupted(error));
        } else {
            GAMENET_TEST_ASSERT(n == static_cast<ssize_t>(payload.size()));
        }
        postDestroyPeerWriteAttempted = true;

        connection.reset();
        outerConnectionReleased = true;
        loop.runAfter(50ms, [&] { loop.quit(); });
    });

    loop.runAfter(0ms, [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->forceClose();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "timed out waiting for repeated connectDestroyed stale-registration check");

    gamenet::test::assertSingleConnectDisconnectClose(callbacks);
    GAMENET_TEST_ASSERT(postDestroyPeerWriteAttempted);
    GAMENET_TEST_ASSERT(outerConnectionReleased);
    GAMENET_TEST_ASSERT(!connection);
    GAMENET_TEST_ASSERT(messageCallbackCount == 0);

    return 0;
}
