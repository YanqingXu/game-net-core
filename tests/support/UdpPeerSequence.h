// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../src/udp/UdpPeerTestAccess.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/udp/UdpPeer.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include <array>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>

namespace gamenet::test {
inline void verifyPeerLedger(const udp::UdpPeerSnapshot& s) {
    GAMENET_TEST_ASSERT(s.acceptedDatagrams == s.sentDatagrams + s.cancelledDatagrams +
                                                   s.failedDatagrams + s.pendingDatagrams);
    GAMENET_TEST_ASSERT(s.acceptedBytes ==
                        s.sentBytes + s.cancelledBytes + s.failedBytes + s.pendingBytes);
    GAMENET_TEST_ASSERT(s.opened == s.closed + s.peers);
    if (s.phase == udp::UdpPhase::Stopped)
        GAMENET_TEST_ASSERT(!s.peers && !s.pendingDatagrams && !s.pendingBytes &&
                            !s.scheduledPosts && !s.socket.nativeOperations &&
                            !s.socket.receiveStorageBytes);
}
// Each action runs on a real owner turn. Native output is real UDP;
// receive/idle and impossible old receipt injection are explicit non-installed
// test controls.
inline void runUdpPeerSequence(std::span<const std::uint8_t> input) {
    using namespace std::chrono_literals;
    net::EventLoop loop;
    std::array<RawUdp, 3> raw;
    std::vector<std::shared_ptr<udp::UdpPeer>> handles;
    struct Entry {
        udp::UdpPeerIdentity peer;
        std::size_t bytes;
        bool terminal{};
    };
    std::map<std::uint64_t, Entry> sends;
    std::set<udp::UdpPeerIdentity> closed;
    std::uint64_t accepted = 0, bytes = 0, terminals = 0, terminalBytes = 0;
    unsigned reentry = 0, stopped = 0;
    std::shared_ptr<udp::UdpPeerServer> server;
    udp::UdpPeerOptions o;
    o.maxPeers = 4;
    o.maxPendingDatagramsPerPeer = 3;
    o.maxPendingBytesPerPeer = 48;
    o.socket.maxDatagramBytes = 32;
    o.socket.maxPendingDatagrams = 5;
    o.socket.maxPendingBytes = 80;
    o.maxPeersPerTurn = 2;
    o.maxCompletionsPerTurn = 2;
    o.idleTimeout = 24h;
    const auto check = [&] {
        const auto s = server->snapshot();
        verifyPeerLedger(s);
        GAMENET_TEST_ASSERT(s.acceptedDatagrams == accepted && s.acceptedBytes == bytes &&
                            s.pendingDatagrams == accepted - terminals &&
                            s.pendingBytes == bytes - terminalBytes);
        GAMENET_TEST_ASSERT(s.peers <= o.maxPeers && s.pendingDatagrams <= 5 &&
                            s.pendingBytes <= 80 && s.scheduledPosts <= 1);
        GAMENET_TEST_ASSERT(s.maxPeersVisitedPerTurn <= 2 && s.maxCompletionsPerTurn <= 2);
        for (auto& p : handles) {
            const auto [n, b] = udp::detail::UdpPeerTestAccess::pending(*server, p->identity());
            GAMENET_TEST_ASSERT(n <= 3 && b <= 48);
            std::size_t expectedCount = 0, expectedBytes = 0;
            for (const auto& [id, e] : sends) {
                (void)id;
                if (e.peer == p->identity() && !e.terminal) {
                    ++expectedCount;
                    expectedBytes += e.bytes;
                }
            }
            GAMENET_TEST_ASSERT(n == expectedCount && b == expectedBytes);
        }
    };
    const auto send = [&](const std::shared_ptr<udp::UdpPeer>& p, std::string_view payload) {
        const auto r = p->sendDatagram(payload);
        if (r.result == udp::UdpResult::Accepted) {
            GAMENET_TEST_ASSERT(r.id && !sends.contains(r.id));
            sends.emplace(r.id, Entry{p->identity(), payload.size()});
            ++accepted;
            bytes += payload.size();
        } else
            GAMENET_TEST_ASSERT(!r.id);
    };
    server = udp::UdpPeerServer::create(
        &loop, net::InetAddress(0, true),
        {.datagram =
             [&](auto p, auto, bool) {
                 GAMENET_TEST_ASSERT(p->isOpen() && !closed.contains(p->identity()));
                 if (reentry == 1) {
                     reentry = 0;
                     (void)p->close(udp::UdpPeerCloseReason::Normal);
                 } else if (reentry == 2) {
                     reentry = 0;
                     send(p, "");
                 } else if (reentry == 3) {
                     reentry = 0;
                     throw std::runtime_error("peer sequence callback");
                 }
             },
         .sendComplete =
             [&](auto identity, auto c) {
                 auto it = sends.find(c.id);
                 GAMENET_TEST_ASSERT(it != sends.end() && !it->second.terminal &&
                                     it->second.peer == identity && it->second.bytes == c.bytes &&
                                     !closed.contains(identity));
                 it->second.terminal = true;
                 ++terminals;
                 terminalBytes += c.bytes;
                 if (reentry == 4) {
                     reentry = 0;
                     (void)server->stop();
                 }
                 check();
             },
         .closed =
             [&](auto identity, auto) {
                 GAMENET_TEST_ASSERT(closed.insert(identity).second);
                 for (const auto& [id, e] : sends) {
                     (void)id;
                     GAMENET_TEST_ASSERT(e.peer != identity || e.terminal);
                 }
                 check();
             },
         .stopped =
             [&](auto s) {
                 verifyPeerLedger(s);
                 GAMENET_TEST_ASSERT(++stopped == 1 && accepted == terminals &&
                                     bytes == terminalBytes);
                 loop.quit();
             }},
        o);
    const auto weak = udp::detail::UdpPeerTestAccess::state(*server);
    GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::Accepted);
    std::size_t position = 0;
    std::function<void()> step;
    step = [&] {
        if (position >= std::min<std::size_t>(input.size(), 96) ||
            server->snapshot().phase != udp::UdpPhase::Running) {
            (void)server->stop();
            return;
        }
        const auto action = input[position++];
        const auto index = (action >> 4) % 3;
        const auto p = handles.empty() ? nullptr : handles[(action >> 3) % handles.size()];
        switch (action % 10) {
        case 0:
        case 1: {
            auto r = server->openPeer(raw[index].address());
            if (r.result == udp::UdpResult::Accepted)
                handles.push_back(r.peer);
            else
                GAMENET_TEST_ASSERT(!r.peer);
            break;
        }
        case 2:
        case 3:
            if (p)
                send(p, std::string((action >> 3) % 40, 'x'));
            break;
        case 4:
            if (p)
                (void)p->close(udp::UdpPeerCloseReason::Replaced);
            break;
        case 5:
            udp::detail::UdpPeerTestAccess::receive(*server, raw[index].address(), "x",
                                                    action & 128);
            break;
        case 6:
            reentry = (action >> 4) % 5;
            break;
        case 7:
            udp::detail::UdpPeerTestAccess::expire(*server);
            break;
        case 8:
            udp::detail::UdpPeerTestAccess::replay(*server,
                                                   std::numeric_limits<std::uint64_t>::max());
            break;
        case 9:
            if (p) {
                const auto before = p->identity();
                auto hold = p;
                (void)hold->requestClose(udp::UdpPeerCloseReason::GoingAway);
                GAMENET_TEST_ASSERT(hold->identity() == before);
            }
            break;
        }
        check();
        GAMENET_TEST_ASSERT(loop.executor().post(step) == net::PostResult::Accepted);
    };
    GAMENET_TEST_ASSERT(loop.executor().post(step) == net::PostResult::Accepted);
    loop.loop();
    GAMENET_TEST_ASSERT(stopped == 1 && loop.attachedLifecycleNodeCount() == 0);
    for (const auto& p : handles)
        GAMENET_TEST_ASSERT(!p->isOpen() && p->send("stale") == udp::UdpResult::Stopped);
    handles.clear();
    server.reset();
    GAMENET_TEST_ASSERT(weak.expired());
}
} // namespace gamenet::test
