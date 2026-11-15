// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../src/kcp/KcpTestAccess.h"
#include "gamenet/core/net/EventLoop.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include <array>
#include <map>
#include <set>
#include <span>
#include <vector>
namespace gamenet::test {
inline std::uint32_t kcpRead32(const char* p) {
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) |
           (std::uint32_t(b[3]) << 24);
}
inline void kcpWrite32(std::string& b, std::size_t at, std::uint32_t v) {
    for (unsigned n = 0; n < 4; ++n)
        b[at + n] = static_cast<char>(v >> (8 * n));
}
inline std::string kcpPacket(std::uint32_t conv, unsigned command, std::uint32_t sn,
                             std::uint32_t una, std::string_view bytes = {}) {
    std::string p(24, '\0');
    kcpWrite32(p, 0, conv);
    p[4] = static_cast<char>(command);
    p[6] = static_cast<char>(128);
    kcpWrite32(p, 12, sn);
    kcpWrite32(p, 16, una);
    kcpWrite32(p, 20, static_cast<std::uint32_t>(bytes.size()));
    p += bytes;
    return p;
}
inline void runKcpSequence(std::span<const std::uint8_t> input) {
    using namespace std::chrono_literals;
    net::EventLoop loop;
    std::array<RawUdp, 3> raw;
    kcp::KcpOptions o;
    o.maxSessions = 4;
    o.maxSessionsPerTurn = 2;
    o.maxMessagesPerSessionTurn = 2;
    o.maxSessionOpenings = 128;
    o.maxPendingMessagesPerSession = 3;
    o.maxPendingMessages = 5;
    o.maxMessageBytes = 2048;
    o.maxPendingBytesPerSession = 4096;
    o.maxPendingBytes = 8192;
    o.sendTimeout = 1h;
    o.idleTimeout = 24h;
    o.udp.idleTimeout = 24h;
    struct Handle {
        std::shared_ptr<kcp::KcpSession> p;
        unsigned raw;
        std::uint32_t end{}, receive{};
    };
    struct Entry {
        kcp::KcpIdentity peer;
        std::size_t bytes;
        std::uint32_t end;
        bool terminal{}, acked{};
    };
    std::vector<Handle> handles;
    std::map<std::uint64_t, Entry> sends;
    std::set<kcp::KcpIdentity> closed;
    unsigned position = 0, reentry = 0, stopped = 0;
    bool advancedPastDeadline = false;
    std::uint64_t accepted = 0, terminals = 0, bytes = 0, terminalBytes = 0;
    std::shared_ptr<kcp::KcpTransport> server;
    const auto check = [&] {
        if (!server)
            return;
        const auto s = server->snapshot();
        GAMENET_TEST_ASSERT(s.accepted == accepted && s.pendingMessages == accepted - terminals &&
                            s.pendingBytes == bytes - terminalBytes);
        GAMENET_TEST_ASSERT(s.accepted ==
                                s.acknowledged + s.failed + s.cancelled + s.pendingMessages &&
                            s.opened == s.closed + s.sessions);
        GAMENET_TEST_ASSERT(s.sessions <= 4 && s.pendingMessages <= 5 && s.pendingBytes <= 8192 &&
                            s.protocolBytes <= o.maxProtocolBytes && s.scheduledPosts <= 1);
        GAMENET_TEST_ASSERT(s.maxSessionsVisitedPerTurn <= 2 && s.maxMessagesPerTurn <= 4);
    };
    const auto send = [&](Handle& h, std::string_view data) {
        const auto a = h.p->send(data);
        if (a.result == kcp::KcpResult::Accepted) {
            h.end +=
                static_cast<std::uint32_t>(std::max<std::size_t>(1, (data.size() + 1175) / 1176));
            GAMENET_TEST_ASSERT(
                sends.emplace(a.id, Entry{h.p->identity(), data.size(), h.end - 1}).second);
            ++accepted;
            bytes += data.size();
        } else
            GAMENET_TEST_ASSERT(!a.id);
    };
    server = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.message =
             [&](auto p, auto data) {
                 GAMENET_TEST_ASSERT(p->isOpen() && data == "peer" &&
                                     !closed.contains(p->identity()));
                 if (reentry == 1) {
                     reentry = 0;
                     (void)p->close();
                 } else if (reentry == 2) {
                     reentry = 0;
                     for (auto& h : handles)
                         if (h.p == p) {
                             send(h, "");
                             break;
                         }
                 } else if (reentry == 3) {
                     reentry = 0;
                     throw std::runtime_error("KCP reentry");
                 } else if (reentry == 5) {
                     reentry = 0;
                     server.reset();
                 }
             },
         .sendComplete =
             [&](auto id, auto c) {
                 auto it = sends.find(c.id);
                 GAMENET_TEST_ASSERT(it != sends.end() && !it->second.terminal &&
                                     it->second.peer == id && it->second.bytes == c.bytes &&
                                     !closed.contains(id));
                 GAMENET_TEST_ASSERT((c.status == kcp::KcpSendStatus::Acknowledged) ==
                                     it->second.acked);
                 it->second.terminal = true;
                 ++terminals;
                 terminalBytes += c.bytes;
                 if (reentry == 4 && server) {
                     reentry = 0;
                     (void)server->stop();
                 }
                 check();
             },
         .closed =
             [&](auto id, auto) {
                 GAMENET_TEST_ASSERT(closed.insert(id).second);
                 for (const auto& [n, e] : sends) {
                     (void)n;
                     GAMENET_TEST_ASSERT(e.peer != id || e.terminal);
                 }
                 check();
             },
         .stopped =
             [&](auto s) {
                 GAMENET_TEST_ASSERT(++stopped == 1 && accepted == terminals &&
                                     bytes == terminalBytes && !s.sessions && !s.pendingMessages &&
                                     !s.pendingBytes && !s.protocolBytes && !s.scheduledPosts &&
                                     !s.udp.socket.nativeOperations &&
                                     !s.udp.socket.receiveStorageBytes);
                 loop.quit();
             }},
        o);
    const auto weak = kcp::detail::KcpTestAccess::state(*server);
    GAMENET_TEST_ASSERT(server->start() == kcp::KcpResult::Accepted);
    std::function<void()> step;
    step = [&] {
        if (!server)
            return;
        if (position >= std::min<std::size_t>(input.size(), 96) ||
            server->snapshot().phase != udp::UdpPhase::Running) {
            (void)server->stop();
            return;
        }
        const auto action = input[position++];
        const auto index = static_cast<unsigned>((action >> 4) % 3);
        auto* h = handles.empty() ? nullptr : &handles[(action >> 3) % handles.size()];
        switch (action % 12) {
        case 0:
        case 1: {
            auto r = server->openSession(raw[index].address(), position);
            if (r.session)
                handles.push_back({r.session, index});
            break;
        }
        case 2:
        case 3:
            if (h)
                send(*h, std::string((action >> 3) % 2 ? 1800 : 0, 'x'));
            break;
        case 4:
            if (h)
                (void)h->p->requestClose();
            break;
        case 5: // Derive cumulative ACK only from actual native standard PUSH output.
            for (unsigned i = 0; i < 3; ++i) {
                char buffer[1200];
                for (unsigned n = 0; n < 16; ++n) {
                    const auto count = recv(raw[i].fd, buffer, sizeof(buffer), 0);
                    if (count < 0)
                        break;
                    std::size_t at = 0;
                    while (at + 24 <= std::size_t(count)) {
                        const auto conv = kcpRead32(buffer + at), sn = kcpRead32(buffer + at + 12),
                                   len = kcpRead32(buffer + at + 20);
                        if (static_cast<unsigned char>(buffer[at + 4]) == 81) {
                            for (auto& peer : handles)
                                if (!advancedPastDeadline && peer.p->identity().conv == conv &&
                                    peer.raw == i && peer.p->isOpen()) {
                                    // Synthetic trusted peer ACK is a test control, never
                                    // production admission.
                                    auto ack = kcpPacket(conv, 82, sn, sn + 1);
                                    kcpWrite32(ack, 8, kcpRead32(buffer + at + 8));
                                    for (auto& [id, e] : sends)
                                        if (e.peer == peer.p->identity() && e.end <= sn)
                                            e.acked = true;
                                    kcp::detail::KcpTestAccess::input(*server, peer.p->identity(),
                                                                      ack);
                                }
                        }
                        at += 24 + len;
                    }
                }
            }
            break;
        case 6:
            reentry = (action >> 4) % 6;
            break;
        case 7:
            if (action & 128) {
                advancedPastDeadline = true;
                kcp::detail::KcpTestAccess::advance(*server, 3601s);
            } else
                kcp::detail::KcpTestAccess::advance(*server, 20ms);
            break;
        case 8:
            if (h) {
                auto id = h->p->identity();
                auto packet = kcpPacket(id.conv + 1, 81, 0, 0, "old");
                kcp::detail::KcpTestAccess::input(*server, id, packet, action & 128);
            }
            break;
        case 9:
            if (h && h->p->isOpen()) {
                auto packet = kcpPacket(h->p->identity().conv, 81, h->receive++, 0, "peer");
                kcp::detail::KcpTestAccess::input(*server, h->p->identity(), packet);
            }
            break;
        case 10:
            if (h)
                kcp::detail::KcpTestAccess::exhaust(*server, h->p->identity());
            break;
        case 11:
            if (h) {
                auto id = h->p->identity();
                (void)h->p->close();
                kcp::detail::KcpTestAccess::input(*server, id,
                                                  kcpPacket(id.conv, 81, 0, 0, "late"));
            }
            break;
        }
        check();
        if (server)
            GAMENET_TEST_ASSERT(loop.executor().post(step) == net::PostResult::Accepted);
    };
    GAMENET_TEST_ASSERT(loop.executor().post(step) == net::PostResult::Accepted);
    loop.loop();
    GAMENET_TEST_ASSERT(stopped == 1 && !loop.attachedLifecycleNodeCount());
    for (auto& h : handles)
        GAMENET_TEST_ASSERT(!h.p->isOpen() && !h.p->send("stale").id);
    handles.clear();
    server.reset();
    GAMENET_TEST_ASSERT(weak.expired());
}
} // namespace gamenet::test
