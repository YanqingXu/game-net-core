// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp/KcpTransport.h"
#include "support/KcpSequence.h"
#include "support/TestAssert.h"
#include <map>
#include <set>
using namespace gamenet;
using namespace std::chrono_literals;
void run(bool v6) {
    net::EventLoop loop;
    const net::InetAddress bind(v6 ? "::1" : "127.0.0.1", 0);
    kcp::KcpOptions o;
    o.maxSessions = 3;
    o.maxSessionsPerTurn = 1;
    o.maxMessagesPerSessionTurn = 1;
    o.maxPendingMessagesPerSession = 2;
    o.maxPendingMessages = 6;
    o.udp.maxPendingDatagramsPerPeer = 1;
    o.udp.socket.maxPendingDatagrams = 2;
    // One per-peer slot forces actual output refusal for fragmented bursts.
    unsigned generation = 0, stopped = 0, completed = 0, closed = 0, rejections = 0;
    bool fair = false;
    std::shared_ptr<kcp::KcpTransport> a, b, c;
    std::shared_ptr<kcp::KcpSession> hot, cold, remoteHot, remoteCold, old;
    std::map<std::uint64_t, kcp::KcpIdentity> ids;
    std::map<unsigned, unsigned> delivered;
    const auto verify = [](const auto& s) {
        GAMENET_TEST_ASSERT(s.accepted ==
                            s.acknowledged + s.failed + s.cancelled + s.pendingMessages);
        GAMENET_TEST_ASSERT(s.maxSessionsVisitedPerTurn <= 1 && s.maxMessagesPerTurn <= 1);
        if (s.phase == udp::UdpPhase::Stopped)
            GAMENET_TEST_ASSERT(!s.sessions && !s.pendingMessages && !s.pendingBytes &&
                                !s.protocolBytes && !s.scheduledPosts &&
                                !s.udp.socket.nativeOperations);
    };
    const auto done = [&](auto snap) {
        verify(snap);
        if (++stopped == 3)
            loop.quit();
    };
    const auto completion = [&](auto identity, auto receipt) {
        GAMENET_TEST_ASSERT(receipt.status == kcp::KcpSendStatus::Acknowledged);
        auto it = ids.find(receipt.id);
        GAMENET_TEST_ASSERT(it != ids.end() && it->second == identity);
        if (identity == cold->identity())
            fair = ids.size() > 1;
        ids.erase(it);
        ++completed;
        verify(a->snapshot());
    };
    a = kcp::KcpTransport::create(
        &loop, bind,
        {.message =
             [&](auto p, auto bytes) {
                 GAMENET_TEST_ASSERT(p->identity().conv == 100 + generation);
                 const auto expected = std::to_string(generation) + ":" + std::string(12000, 'a');
                 GAMENET_TEST_ASSERT(bytes == expected || bytes.empty());
                 ++delivered[generation];
             },
         .sendComplete = completion,
         .closed = [&](auto, auto) { ++closed; },
         .stopped = done},
        o);
    b = kcp::KcpTransport::create(&loop, bind,
                                  {.message =
                                       [&](auto p, auto bytes) {
                                           GAMENET_TEST_ASSERT(p->send(bytes).result ==
                                                               kcp::KcpResult::Accepted);
                                       },
                                   .stopped = done},
                                  o);
    c = kcp::KcpTransport::create(&loop, bind,
                                  {.message =
                                       [&](auto, auto bytes) {
                                           GAMENET_TEST_ASSERT(bytes == "cold");
                                           ++delivered[99];
                                       },
                                   .stopped = done},
                                  o);
    for (auto p : {a, b, c})
        GAMENET_TEST_ASSERT(p->start() == kcp::KcpResult::Accepted);
    cold = a->openSession(c->localAddress(), 55).session;
    remoteCold = c->openSession(a->localAddress(), 55).session;
    GAMENET_TEST_ASSERT(cold && remoteCold);
    const auto send = [&](const auto& p, std::string_view bytes) {
        auto r = p->send(bytes);
        if (r.result == kcp::KcpResult::Accepted) {
            GAMENET_TEST_ASSERT(ids.emplace(r.id, p->identity()).second);
        }
        return r;
    };
    const auto open = [&] {
        hot = a->openSession(b->localAddress(), 100 + generation).session;
        remoteHot = b->openSession(a->localAddress(), 100 + generation).session;
        GAMENET_TEST_ASSERT(hot && remoteHot);
        if (generation) {
            GAMENET_TEST_ASSERT(kcp::detail::KcpTestAccess::wire(
                                    *b, remoteHot->identity(),
                                    test::kcpPacket(100 + generation - 1, 81, 0, 0, "stale"))
                                    .result == kcp::KcpResult::Accepted);
            kcp::detail::KcpTestAccess::input(
                *a, old->identity(), test::kcpPacket(100 + generation - 1, 81, 0, 0, "late"));
        }
        GAMENET_TEST_ASSERT(
            send(hot, std::to_string(generation) + ":" + std::string(12000, 'a')).id);
        GAMENET_TEST_ASSERT(send(hot, "").id);
        for (unsigned n = 0; n < 8; ++n) {
            auto r = send(hot, "rejected");
            GAMENET_TEST_ASSERT(r.result == kcp::KcpResult::Overloaded && !r.id);
            ++rejections;
        }
    };
    open();
    GAMENET_TEST_ASSERT(send(cold, "cold").id);
    loop.runEvery(5ms, [&] {
        verify(a->snapshot());
        if (delivered[generation] == 2 && ids.empty()) {
            old = hot;
            GAMENET_TEST_ASSERT(hot->close() == kcp::KcpResult::Accepted);
            GAMENET_TEST_ASSERT(remoteHot->close() == kcp::KcpResult::Accepted);
            if (++generation == 4) {
                (void)a->stop();
                (void)b->stop();
                (void)c->stop();
                return;
            }
            GAMENET_TEST_ASSERT(old->send("stale").result == kcp::KcpResult::Stopped);
            GAMENET_TEST_ASSERT(a->openSession(b->localAddress(), 100 + generation - 1).result ==
                                kcp::KcpResult::InvalidArgument);
            open();
        }
    });
    loop.runAfter(20s, [&] {
        std::cerr << "g=" << generation << " delivered=" << delivered[generation]
                  << " completions=" << completed << " pending=" << ids.size()
                  << " drops=" << a->snapshot().outputDrops << std::endl;
        GAMENET_TEST_FAIL("KCP real UDP pressure timeout");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped == 3 && generation == 4 && completed == 9 && delivered[99] == 1 &&
                        closed == 5 && rejections == 32);
    GAMENET_TEST_ASSERT(fair && a->snapshot().outputDrops > 0 &&
                        a->snapshot().invalidDatagrams >= 3 && !loop.attachedLifecycleNodeCount());
}
int main() {
    run(false);
    run(true);
}
