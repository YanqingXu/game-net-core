// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/core/net/EventLoop.h>
#ifdef CONSUMER_UDP
#include <gamenet/udp/UdpPeer.h>
#include <gamenet/udp/UdpSocket.h>
#endif
#ifdef CONSUMER_UDP_TRANSPORT
#include <gamenet/udp_transport/UdpTransportEndpoint.h>
#endif
int main() {
    gamenet::net::EventLoop loop;
#ifdef CONSUMER_UDP
    bool stopped = false;
    auto socket = gamenet::udp::UdpPeerServer::create(
        &loop, gamenet::net::InetAddress(0, true), {.stopped = [&](auto s) {
            stopped = s.phase == gamenet::udp::UdpPhase::Stopped && !s.socket.nativeOperations &&
                      !s.pendingBytes && !s.peers;
            loop.quit();
        }});
    if (socket->start() != gamenet::udp::UdpResult::Accepted)
        return 1;
    auto peer = socket->openPeer(gamenet::net::InetAddress(9, true)).peer;
#ifdef CONSUMER_UDP_TRANSPORT
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint =
        std::make_shared<gamenet::udp_transport::UdpTransportEndpoint>(peer);
    if (!endpoint || !endpoint->isOpen() ||
        endpoint->send("") != gamenet::transport::EndpointResult::Accepted)
        return 3;
#else
    if (!peer || peer->sendDatagram("").result != gamenet::udp::UdpResult::Accepted)
        return 3;
#endif
    (void)socket->stop();
    loop.loop();
    return stopped ? 0 : 2;
#else
    return 0;
#endif
}
