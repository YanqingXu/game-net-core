#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct ClientArgs {
    std::string ip;
    std::uint16_t port{7000};
    std::string message{"hello"};
};

[[noreturn]] void printUsageAndExit() {
    std::cerr << "usage: echo_client <ip> <port> [message]\n";
    std::exit(2);
}

std::uint16_t parsePort(const std::string& portText) {
    std::size_t consumed = 0;
    const int port = std::stoi(portText, &consumed);
    if (consumed != portText.size() || port <= 0 || port > 65535) {
        throw std::out_of_range("port out of range");
    }
    return static_cast<std::uint16_t>(port);
}

ClientArgs parseArgs(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        printUsageAndExit();
    }

    try {
        ClientArgs args;
        args.ip = argv[1];
        args.port = parsePort(argv[2]);
        if (argc == 4) {
            args.message = argv[3];
        }
        return args;
    } catch (const std::exception&) {
        printUsageAndExit();
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const ClientArgs args = parseArgs(argc, argv);

    gamenet::net::EventLoop loop;
    gamenet::net::TcpClient client(&loop, gamenet::net::InetAddress(args.ip, args.port), "echo_client");

    bool echoed = false;

    client.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            std::cout << "connected " << connection->peerAddress().toIpPort() << '\n';
            connection->send(args.message);
            return;
        }

        std::cout << "disconnected\n";
    });

    client.setMessageCallback([&](const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* buffer) {
        const std::string reply = buffer->retrieveAllAsString();
        std::cout << reply << '\n';

        echoed = (reply == args.message);
        connection->shutdown();
        client.stop();
        loop.quit();
    });

    loop.runAfter(std::chrono::seconds(5), [&] {
        std::cerr << "echo_client timed out\n";
        client.stop();
        loop.quit();
    });

    client.connect();
    loop.loop();

    return echoed ? 0 : 1;
}
