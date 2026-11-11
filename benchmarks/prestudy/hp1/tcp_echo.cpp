// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerViewReceiver.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpServer.h"

#include <any>
#include <charconv>
#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    unsigned port = 7005;
    if (argc > 2) return 2;
    if (argc == 2) {
        const auto end = argv[1] + std::strlen(argv[1]);
        const auto parsed = std::from_chars(argv[1], end, port);
        if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535) {
            std::cerr << "usage: gamenet_hp1_borrowed_tcp_echo [port]\n";
            return 2;
        }
    }
    using namespace gamenet::experimental::hp1;
    gamenet::net::EventLoop loop;
    gamenet::protocol::PacketFramer encoder;
    gamenet::net::TcpServer server(
        &loop, gamenet::net::InetAddress(static_cast<std::uint16_t>(port)), "hp1_borrowed_echo");
    server.setThreadNum(0);
    server.setConnectionCallback([&](const auto& connection) {
        if (connection->connected()) {
            connection->setContext(PacketFramerViewReceiver::create(
                connection, [&](const auto& current, PacketView view) {
                    const auto reply = encoder.encode(view.asStringView());
                    if (!reply || current->trySend(*reply) != gamenet::net::TcpSendResult::Accepted) {
                        current->forceClose();
                        return FrameVisitAction::Stop;
                    }
                    return FrameVisitAction::Continue;
                }));
        } else {
            const auto* state = std::any_cast<std::shared_ptr<PacketFramerViewReceiver>>(
                &connection->getContext());
            if (state && *state) {
                const auto receiver = *state;
                receiver->stop();
            }
            connection->setContext(std::any{});
        }
    });
    server.setMessageCallback([](const auto& connection, gamenet::net::Buffer* input) {
        const auto* state = std::any_cast<std::shared_ptr<PacketFramerViewReceiver>>(
            &connection->getContext());
        if (state && *state) {
            const auto receiver = *state;
            receiver->onMessage(*input);
        } else input->retrieveAll();
    });
    server.start();
    std::cout << "HP1 borrowed TCP echo listening on " << server.listenAddress().toIpPort() << std::endl;
    loop.loop();
}
