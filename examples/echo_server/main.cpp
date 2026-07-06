#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::uint16_t parsePort(int argc, char* argv[]) {
    if (argc <= 1) {
        return 7000;
    }

    try {
        if (argc > 2) {
            throw std::invalid_argument("too many arguments");
        }
        const std::string portText(argv[1]);
        std::size_t consumed = 0;
        const int port = std::stoi(portText, &consumed);
        if (consumed != portText.size()) {
            throw std::invalid_argument("invalid port");
        }
        if (port <= 0 || port > 65535) {
            throw std::out_of_range("port out of range");
        }
        return static_cast<std::uint16_t>(port);
    } catch (const std::exception&) {
        std::cerr << "usage: echo_server [port]\n";
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::uint16_t port = parsePort(argc, argv);

    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(port), "echo_server");

    server.setConnectionCallback([](const gamenet::net::TcpConnectionPtr& connection) {
        std::cout << (connection->connected() ? "connected " : "disconnected ")
                  << connection->peerAddress().toIpPort() << '\n';
    });

    server.setMessageCallback([](const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* buffer) {
        connection->send(buffer->retrieveAllAsString());
    });

    server.start();

    std::cout << "echo_server listening on " << server.listenAddress().toIpPort() << '\n';
    loop.loop();
    return 0;
}
