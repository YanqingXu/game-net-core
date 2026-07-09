#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include <any>
#include <memory>
#include <string>

int main() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "contract#1");

    bool connectedCallback = false;
    bool messageCallback = false;
    bool writeCompleteCallback = false;
    bool closeCallback = false;

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connectedCallback = true;
        }
    });
    connection->setMessageCallback([&](const gamenet::net::TcpConnectionPtr& conn, gamenet::net::Buffer* buffer) {
        messageCallback = true;
        GAMENET_TEST_ASSERT(buffer->retrieveAllAsString() == "ping");
        conn->send("pong");
    });
    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        writeCompleteCallback = true;
        conn->forceClose();
    });
    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        closeCallback = true;
        conn->connectDestroyed();
        loop.quit();
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        connection->connectEstablished();
        GAMENET_TEST_ASSERT(connection->connected());

        connection->setContext(std::string("owner-loop-context"));
        const auto* mutableContextValue = std::any_cast<std::string>(&connection->getContext());
        GAMENET_TEST_ASSERT(mutableContextValue != nullptr);
        GAMENET_TEST_ASSERT(*mutableContextValue == "owner-loop-context");

        const gamenet::net::TcpConnection& constConnection = *connection;
        const auto* constContextValue = std::any_cast<std::string>(&constConnection.getContext());
        GAMENET_TEST_ASSERT(constContextValue != nullptr);
        GAMENET_TEST_ASSERT(*constContextValue == "owner-loop-context");

        const std::string payload = "ping";
        const auto written = gamenet::net::sockets::write(pair.peerFd, payload.data(), payload.size());
        GAMENET_TEST_ASSERT(written == static_cast<ssize_t>(payload.size()));
    });

    loop.loop();

    GAMENET_TEST_ASSERT(connectedCallback);
    GAMENET_TEST_ASSERT(messageCallback);
    GAMENET_TEST_ASSERT(writeCompleteCallback);
    GAMENET_TEST_ASSERT(closeCallback);
    GAMENET_TEST_ASSERT(connection->disconnected());

    char response[16] = {};
    const auto readBytes = gamenet::net::sockets::read(pair.peerFd, response, sizeof(response));
    GAMENET_TEST_ASSERT(readBytes == 4);
    GAMENET_TEST_ASSERT(std::string(response, 4) == "pong");

    return 0;
}
