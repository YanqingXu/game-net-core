// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/udp/UdpPeer.h"
#include <iostream>
#include <string>
// Diagnostic example: explicit source-port registration; local generation only.
int main(int argc, char** argv) {
    using namespace gamenet;
    if (argc != 3)
        return 2;
    const bool ipv6 = std::string(argv[1]) == "6";
    const std::string host = ipv6 ? "::1" : "127.0.0.1";
    net::EventLoop loop;
    unsigned received = 0, sent = 0, closed = 0, truncated = 0;
    bool failed = false;
    std::shared_ptr<udp::UdpPeerServer> server;
    std::shared_ptr<udp::UdpPeer> old, current;
    udp::UdpPeerOptions o;
    o.maxPeers = 2;
    o.socket.maxDatagramBytes = 64;
    server = udp::UdpPeerServer::create(
        &loop, net::InetAddress(host, 0),
        {.datagram =
             [&](auto peer, auto bytes, bool cut) {
                 ++received;
                 if (cut)
                     ++truncated;
                 if (bytes == "rotate") {
                     old = peer;
                     (void)old->close(transport::CloseReason::Replaced);
                     current = server->openPeer(peer->address()).peer;
                     if (!current || current->id() == old->id() ||
                         old->send("stale") != transport::EndpointResult::Closed) {
                         failed = true;
                         (void)server->stop();
                         return;
                     }
                     if (current->send("rotated") != transport::EndpointResult::Accepted) {
                         failed = true;
                         (void)server->stop();
                     }
                 } else if (peer->send(bytes) != transport::EndpointResult::Accepted) {
                     failed = true;
                     (void)server->stop();
                 }
             },
         .sendComplete =
             [&](auto, auto c) {
                 if (c.status != udp::UdpSendStatus::Sent)
                     failed = true;
                 if (++sent == 8)
                     (void)server->stop();
             },
         .closed = [&](auto, auto) { ++closed; },
         .stopped =
             [&](auto s) {
                 if (s.peers || s.pendingDatagrams || s.pendingBytes || s.scheduledPosts ||
                     s.socket.nativeOperations || s.socket.receiveStorageBytes ||
                     s.acceptedDatagrams !=
                         s.sentDatagrams + s.cancelledDatagrams + s.failedDatagrams)
                     failed = true;
                 std::cout << "PEER_SUMMARY " << received << ' ' << sent << ' ' << closed << ' '
                           << truncated << ' ' << s.unknownDatagrams << ' ' << s.cancelledDatagrams
                           << ' ' << s.failedDatagrams << ' ' << (failed ? "failed" : "passed")
                           << std::endl;
                 loop.quit();
             }},
        o);
    if (server->start() != udp::UdpResult::Accepted)
        return 3;
    current =
        server->openPeer(net::InetAddress(host, static_cast<std::uint16_t>(std::stoul(argv[2]))))
            .peer;
    if (!current)
        return 4;
    std::cout << "PORT " << server->localAddress().port() << std::endl;
    loop.runAfter(std::chrono::seconds(10), [&] {
        failed = true;
        (void)server->stop();
    });
    loop.loop();
    return failed ? 5 : 0;
}
