#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TestAssert.h"
#include <chrono>
#include <memory>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;

    const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
    const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

    auto connection = std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        "write-complete-after-send-return",
        pair.connectionFd,
        localAddr,
        peerAddr);

    bool connectedCallback = false;
    bool sendReturned = false;
    bool writeCompleteBeforeSendReturned = false;
    bool writeCompleteAfterSendReturned = false;
    int writeCompleteCallbackCount = 0;
    int closeCallbackCount = 0;

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            connectedCallback = true;
        }
    });

    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbackCount;
        if (sendReturned) {
            writeCompleteAfterSendReturned = true;
        } else {
            writeCompleteBeforeSendReturned = true;
        }
        conn->forceClose();
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    const std::string payload = "write-complete-after-send-return";
    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->send(payload);
        sendReturned = true;
        GAMENET_TEST_ASSERT(!writeCompleteBeforeSendReturned);
    });

    loop.runAfter(std::chrono::seconds(1), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for write-complete callback");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(connectedCallback);
    GAMENET_TEST_ASSERT(sendReturned);
    GAMENET_TEST_ASSERT(!writeCompleteBeforeSendReturned);
    GAMENET_TEST_ASSERT(writeCompleteAfterSendReturned);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

    char response[64] = {};
    const auto readBytes = gamenet::net::sockets::read(pair.peerFd, response, sizeof(response));
    GAMENET_TEST_ASSERT(readBytes == static_cast<ssize_t>(payload.size()));
    GAMENET_TEST_ASSERT(std::string(response, static_cast<std::size_t>(readBytes)) == payload);

    return 0;
}
