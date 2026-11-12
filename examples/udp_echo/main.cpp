// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/udp/UdpSocket.h"
#include "gamenet/core/net/EventLoop.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    using namespace gamenet;
    try {
        const bool ipv6 = argc > 1 && std::string(argv[1]) == "6";
        const auto port = argc > 2 ? static_cast<std::uint16_t>(std::stoul(argv[2])) : 0;
        const auto target = argc > 3 ? std::stoul(argv[3]) : 0;
        udp::UdpOptions options;
        if (argc > 4)
            options.maxDatagramBytes = std::stoul(argv[4]);
        options.validate();
        net::EventLoop loop;
        std::uint64_t received = 0, sent = 0, truncated = 0;
        bool okay = true;
        std::shared_ptr<udp::UdpSocket> server;
        server = udp::UdpSocket::create(
            &loop, net::InetAddress(ipv6 ? "::1" : "127.0.0.1", port),
            {.datagram =
                 [&](const auto& peer, auto bytes, bool cut) {
                     ++received;
                     if (cut)
                         ++truncated;
                     if (server->sendTo(peer, bytes).result != udp::UdpResult::Accepted) {
                         okay = false;
                         (void)server->stop();
                     }
                 },
             .sendComplete =
                 [&](auto completion) {
                     if (completion.status != udp::UdpSendStatus::Sent)
                         okay = false;
                     else if (++sent == target && target)
                         (void)server->stop();
                 },
             .stopped =
                 [&](auto summary) {
                     okay = okay && summary.nativeOperations == 0 &&
                            summary.pendingDatagrams == 0 && summary.pendingBytes == 0 &&
                            summary.receiveStorageBytes == 0 && summary.failedDatagrams == 0;
                     std::cout << "SUMMARY " << received << ' ' << sent << ' ' << truncated << ' '
                               << summary.cancelledDatagrams << '\n'
                               << std::flush;
                     loop.quit();
                 }},
            options);
        if (server->start() != udp::UdpResult::Accepted)
            return 2;
        std::cout << "PORT " << server->localAddress().port() << '\n' << std::flush;
        loop.loop();
        return okay ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
