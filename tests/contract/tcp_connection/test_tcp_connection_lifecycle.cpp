#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cassert>
#include <memory>
#include <string>

#ifdef _WIN32
#include <array>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

struct ConnectedPair {
    gamenet::net::SocketFd connectionFd{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd peerFd{gamenet::net::kInvalidSocket};

    ConnectedPair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        connectionFd = fds[0];
        peerFd = fds[1];
#else
        int fds[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        connectionFd = fds[0];
        peerFd = fds[1];
#endif
    }

    ~ConnectedPair() {
        gamenet::net::sockets::close(peerFd);
    }
};

}  // namespace

int main() {
    gamenet::net::EventLoop loop;
    ConnectedPair pair;

    const gamenet::net::InetAddress localAddr(gamenet::net::sockets::getLocalAddr(pair.connectionFd));
    const gamenet::net::InetAddress peerAddr(gamenet::net::sockets::getPeerAddr(pair.connectionFd));

    auto connection = std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        "contract#1",
        pair.connectionFd,
        localAddr,
        peerAddr);

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
        assert(buffer->retrieveAllAsString() == "ping");
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
        assert(connection->connected());

        const std::string payload = "ping";
        const auto written = gamenet::net::sockets::write(pair.peerFd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));
    });

    loop.loop();

    assert(connectedCallback);
    assert(messageCallback);
    assert(writeCompleteCallback);
    assert(closeCallback);
    assert(connection->disconnected());

    char response[16] = {};
    const auto readBytes = gamenet::net::sockets::read(pair.peerFd, response, sizeof(response));
    assert(readBytes == 4);
    assert(std::string(response, 4) == "pong");

    return 0;
}
