// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/UdpPeerSequence.h"
#include <set>
using namespace gamenet;
using namespace std::chrono_literals;
static void generation(unsigned generation) {
    net::EventLoop loop;
    test::RawUdp hot(generation % 2), cold(generation % 2);
    udp::UdpPeerOptions o;
    o.maxPeers = 3;
    o.maxPendingDatagramsPerPeer = 4;
    o.maxPendingBytesPerPeer = 128;
    o.socket.maxDatagramBytes = 32;
    o.socket.maxPendingDatagrams = 8;
    o.socket.maxPendingBytes = 256;
    o.maxPeersPerTurn = 1;
    o.maxCompletionsPerTurn = 1;
    std::shared_ptr<udp::UdpPeer> a, b, replacement;
    std::shared_ptr<udp::UdpPeerServer> server;
    unsigned done = 0, closed = 0, stage = 0, ticks = 0, rejections = 0;
    bool stopped = false;
    std::set<std::string> received;
    std::set<std::uint64_t> ids;
    const std::string prefix = std::to_string(generation) + ":";
    server = udp::UdpPeerServer::create(
        &loop, net::InetAddress(generation % 2 ? "::1" : "127.0.0.1", 0),
        {.sendComplete =
             [&](auto identity, auto c) {
                 GAMENET_TEST_ASSERT(ids.insert(c.id).second);
                 ++done;
                 if (done == 2)
                     GAMENET_TEST_ASSERT(identity == b->identity());
                 test::verifyPeerLedger(server->snapshot());
             },
         .closed = [&](auto, auto) { ++closed; },
         .stopped =
             [&](auto s) {
                 test::verifyPeerLedger(s);
                 GAMENET_TEST_ASSERT(s.maxPeersVisitedPerTurn <= 1 && s.maxCompletionsPerTurn <= 1);
                 stopped = true;
                 loop.quit();
             }},
        o);
    const auto weak = udp::detail::UdpPeerTestAccess::state(*server);
    GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::Accepted);
    a = server->openPeer(hot.address()).peer;
    b = server->openPeer(cold.address()).peer;
    for (unsigned n = 0; n < 20; ++n) {
        const auto r = a->sendDatagram(prefix + "hot" + std::to_string(n));
        if (n < 4)
            GAMENET_TEST_ASSERT(r.result == udp::UdpResult::Accepted);
        else {
            GAMENET_TEST_ASSERT(r.result == udp::UdpResult::Overloaded && !r.id);
            ++rejections;
        }
    }
    GAMENET_TEST_ASSERT(b->sendDatagram(prefix + "cold").result == udp::UdpResult::Accepted);
    loop.runEvery(1ms, [&] {
        ++ticks;
        for (auto* raw : {&hot, &cold}) {
            char data[64];
            for (unsigned n = 0; n < 16; ++n) {
                const auto count = ::recv(raw->fd, data, sizeof(data), 0);
                if (count < 0)
                    break;
                GAMENET_TEST_ASSERT(
                    received.insert(std::string(data, static_cast<std::size_t>(count))).second);
            }
        }
        if (stage == 0 && done == 5 && received.size() == 5) {
            stage = 1;
            for (unsigned n = 0; n < 4; ++n)
                GAMENET_TEST_ASSERT(a->sendDatagram(prefix + "cancel" + std::to_string(n)).result ==
                                    udp::UdpResult::Accepted);
            GAMENET_TEST_ASSERT(a->close(transport::CloseReason::Replaced) ==
                                transport::EndpointResult::Accepted);
            replacement = server->openPeer(hot.address()).peer;
            GAMENET_TEST_ASSERT(replacement && replacement->id() != a->id());
            GAMENET_TEST_ASSERT(a->send("stale") == transport::EndpointResult::Closed);
            udp::detail::UdpPeerTestAccess::replay(*server,
                                                   std::numeric_limits<std::uint64_t>::max());
            GAMENET_TEST_ASSERT(replacement->sendDatagram(prefix + "recovered").result ==
                                udp::UdpResult::Accepted);
        } else if (stage == 1 && done == 10 && received.contains(prefix + "recovered") &&
                   closed == 1) {
            stage = 2;
            (void)server->stop();
        }
    });
    loop.runAfter(5s, [&] {
        const auto s = server->snapshot();
        std::cerr << "generation=" << generation << " stage=" << stage << " done=" << done
                  << " received=" << received.size() << " closed=" << closed << " peers=" << s.peers
                  << " pending=" << s.pendingDatagrams << " posted=" << s.scheduledPosts
                  << " native=" << s.socket.pendingDatagrams
                  << " nativeOps=" << s.socket.nativeOperations << std::endl;
        GAMENET_TEST_FAIL("UDP peer pressure timeout");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped && done == 10 && closed == 3 && ticks && rejections == 16);
    server.reset();
    GAMENET_TEST_ASSERT(weak.expired() && !a->isOpen() && !replacement->isOpen());
}
int main() {
    for (unsigned n = 0; n < 16; ++n)
        generation(n);
}
