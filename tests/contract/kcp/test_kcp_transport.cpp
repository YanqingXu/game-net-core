// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp/KcpTransport.h"
#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopLifecycleRegistry.h"
#include "../../../src/udp/UdpPeerTestAccess.h"
#include "support/KcpSequence.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include <set>
#include <thread>
using namespace gamenet;
using namespace std::chrono_literals;
void ledger(const kcp::KcpSnapshot& s) {
    GAMENET_TEST_ASSERT(s.accepted == s.acknowledged + s.cancelled + s.failed + s.pendingMessages);
    GAMENET_TEST_ASSERT(s.opened == s.closed + s.sessions);
    test::verifyUdpLedger(s.udp.socket);
    if (s.phase == udp::UdpPhase::Stopped)
        GAMENET_TEST_ASSERT(!s.sessions && !s.pendingMessages && !s.pendingBytes &&
                            !s.protocolBytes && !s.scheduledPosts &&
                            !s.udp.peers && !s.udp.pendingDatagrams && !s.udp.pendingBytes &&
                            !s.udp.socket.nativeOperations && !s.udp.socket.receiveStorageBytes);
}

enum class CallbackClose { Stop, ForeignStop, Quit, ForeignQuit, Release, Throw, SessionClose };
void callbackCloseAtSendDeadline(CallbackClose action, bool acknowledgment) {
    net::EventLoop loop;
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.maxSessions = o.maxSessionsPerTurn = 1;
    o.udp.maxPeers = o.udp.maxPeersPerTurn = 1;
    o.idleTimeout = o.udp.idleTimeout = 1h;
    unsigned reentered = 0, received = 0, acked = 0, cancelled = 0, closed = 0, stopped = 0;
    std::set<std::uint64_t> terminals;
    std::shared_ptr<kcp::KcpTransport> s;
    std::shared_ptr<kcp::KcpSession> p;
    std::weak_ptr<void> kcpTimer, udpTimer;
    net::TimerId watchdog, pump;
    auto close = [&] {
        GAMENET_TEST_ASSERT(++reentered == 1);
        switch (action) {
            case CallbackClose::Stop:
                GAMENET_TEST_ASSERT(s->stop() == kcp::KcpResult::Accepted);
                break;
            case CallbackClose::ForeignStop: {
                std::thread foreign([&] {
                    GAMENET_TEST_ASSERT(s->stop() == kcp::KcpResult::Accepted);
                });
                foreign.join();
                break;
            }
            case CallbackClose::Quit: loop.quit(); break;
            case CallbackClose::ForeignQuit: {
                std::thread foreign([&] { loop.quit(); });
                foreign.join();
                break;
            }
            case CallbackClose::Release: s.reset(); break;
            case CallbackClose::Throw: throw std::runtime_error("KCP callback stop at deadline");
            case CallbackClose::SessionClose:
                GAMENET_TEST_ASSERT(p->close() == kcp::KcpResult::Accepted);
                break;
        }
        GAMENET_TEST_ASSERT(!p->isOpen());
    };
    s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.message = [&](auto peer, auto bytes) {
             GAMENET_TEST_ASSERT(!acknowledgment && peer == p && bytes == "close");
             ++received;
             close();
         },
         .sendComplete = [&](auto id, auto c) {
             GAMENET_TEST_ASSERT(id == p->identity() && terminals.insert(c.id).second);
             if (c.status == kcp::KcpSendStatus::Acknowledged) {
                 GAMENET_TEST_ASSERT(acknowledgment && !acked++);
                 close();
             } else {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Cancelled);
                 ++cancelled;
             }
         },
         .closed = [&](auto, auto reason) {
             const auto expected = action == CallbackClose::SessionClose
                                       ? kcp::KcpCloseReason::Normal : kcp::KcpCloseReason::GoingAway;
             GAMENET_TEST_ASSERT(reason == expected && ++closed == 1);
             if (s) (void)s->stop();
         },
         .stopped = [&](auto snap) {
             ledger(snap);
             GAMENET_TEST_ASSERT(snap.accepted == (acknowledgment ? 2 : 1) && !snap.failed &&
                                 snap.acknowledged == unsigned(acknowledgment) && snap.cancelled == 1 &&
                                 snap.callbackExceptions == unsigned(action == CallbackClose::Throw) &&
                                 kcpTimer.expired() && udpTimer.expired() && ++stopped == 1);
             net::detail::EventLoopLifecycleRegistry::retireTimer(loop, pump);
             net::detail::EventLoopLifecycleRegistry::retireTimer(loop, watchdog);
             loop.quit();
         }}, o);
    const auto weak = kcp::detail::KcpTestAccess::state(*s);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    kcpTimer = kcp::detail::KcpTestAccess::timerLifetime(*s);
    udpTimer = udp::detail::UdpPeerTestAccess::timerLifetime(kcp::detail::KcpTestAccess::udpServer(*s));
    p = s->openSession(raw.address(), 208).session;
    GAMENET_TEST_ASSERT(p && p->send("pending").id);
    if (acknowledgment) {
        GAMENET_TEST_ASSERT(p->send("unacknowledged").id);
        kcp::detail::KcpTestAccess::advance(*s, 0ms);
        pump = loop.runEvery(1ms, [&] {
            if (reentered) return;
            char wire[1200];
            const auto length = recv(raw.fd, wire, sizeof(wire), 0);
            if (length < 24) return;
            auto ack = test::kcpPacket(208, 82, 0, 1);
            test::kcpWrite32(ack, 8, test::kcpRead32(wire + 8));
            kcp::detail::KcpTestAccess::input(*s, p->identity(), ack);
            kcp::detail::KcpTestAccess::advance(*s, 11s);
        });
    } else {
        kcp::detail::KcpTestAccess::input(*s, p->identity(), test::kcpPacket(208, 81, 0, 0, "close"));
        kcp::detail::KcpTestAccess::advance(*s, 11s);
    }
    watchdog = loop.runAfter(3s, [] { GAMENET_TEST_FAIL("KCP callback/deadline close did not settle"); });
    loop.loop();
    GAMENET_TEST_ASSERT(reentered == 1 && received == unsigned(!acknowledgment) &&
                        acked == unsigned(acknowledgment) && cancelled == 1 && closed == 1 &&
                        stopped == 1 && !p->isOpen() && !loop.attachedLifecycleNodeCount());
    s.reset();
    GAMENET_TEST_ASSERT(weak.expired() && kcpTimer.expired() && udpTimer.expired());
}

void quitRetiresTimers(bool foreign, bool saturated, bool readyBatch = false) {
    net::EventLoop loop({.maxPendingFunctors = 4, .reservedPendingFunctors = 0,
                         .maxFunctorsPerIteration = 1});
    test::RawUdp raw;
    unsigned completed = 0, closed = 0, stopped = 0;
    std::weak_ptr<void> kcpTimer, udpTimer;
    auto s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete = [&](auto, auto c) {
             GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Cancelled);
             ++completed;
         },
         .closed = [&](auto, auto reason) {
             GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::GoingAway);
             ++closed;
         },
         .stopped = [&](auto snap) {
             ledger(snap);
             GAMENET_TEST_ASSERT(snap.accepted == 2 && snap.cancelled == 2 &&
                                 kcpTimer.expired() && udpTimer.expired());
             ++stopped;
         }});
    const auto weak = kcp::detail::KcpTestAccess::state(*s);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    kcpTimer = kcp::detail::KcpTestAccess::timerLifetime(*s);
    udpTimer = udp::detail::UdpPeerTestAccess::timerLifetime(
        kcp::detail::KcpTestAccess::udpServer(*s));
    GAMENET_TEST_ASSERT(!kcpTimer.expired() && !udpTimer.expired());
    auto p = s->openSession(raw.address(), 207).session;
    GAMENET_TEST_ASSERT(p && p->send("pending").id && p->send("").id);
    if (saturated)
        while (loop.executor().post([] {}) == net::PostResult::Accepted) {}
    if (readyBatch) {
        // quit runs first, followed by the already-extracted repeating ticks.
        loop.runAt(base::now() - 1h, [&] { loop.quit(); });
        net::detail::EventLoopActiveBatchHarness::runExpiredTimers(loop, base::now() + 1s);
    } else if (foreign) {
        std::thread quitter([&] { loop.quit(); });
        quitter.join();
    } else
        loop.quit();
    loop.loop();
    GAMENET_TEST_ASSERT(completed == 2 && closed == 1 && stopped == 1 &&
                        !loop.attachedLifecycleNodeCount());
    s.reset();
    GAMENET_TEST_ASSERT(weak.expired() && !p->isOpen() &&
                        kcpTimer.expired() && udpTimer.expired());
}

enum class IdleOrder { UdpFirst, KcpFirst, ExplicitCloseFirst };
void idleCloseInterleaving(IdleOrder order) {
    net::EventLoop loop;
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.maxSessions = o.maxSessionsPerTurn = 1;
    o.udp.maxPeers = o.udp.maxPeersPerTurn = 1;
    o.interval = 100ms;
    o.idleTimeout = o.udp.idleTimeout = 1h;
    const auto expected = order == IdleOrder::ExplicitCloseFirst ? kcp::KcpCloseReason::Normal
                                                               : kcp::KcpCloseReason::IdleTimeout;
    const unsigned accepted = order == IdleOrder::KcpFirst ? 0 : 2;
    unsigned completed = 0, closed = 0, stopped = 0;
    std::weak_ptr<void> kcpTimer, udpTimer;
    net::TimerId watchdog;
    std::shared_ptr<kcp::KcpTransport> s;
    s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete = [&](auto, auto c) {
             const auto status = expected == kcp::KcpCloseReason::Normal
                                     ? kcp::KcpSendStatus::Cancelled : kcp::KcpSendStatus::Failed;
             GAMENET_TEST_ASSERT(c.status == status);
             ++completed;
         },
         .closed = [&](auto, auto reason) {
             GAMENET_TEST_ASSERT(reason == expected && s->snapshot().udp.idleClosed == 1);
             ++closed;
             (void)s->stop();
         },
         .stopped = [&](auto snap) {
             ledger(snap);
             GAMENET_TEST_ASSERT(snap.accepted == accepted);
             ++stopped;
             GAMENET_TEST_ASSERT(loop.tryCancel(watchdog) == net::PostResult::Accepted);
             GAMENET_TEST_ASSERT(kcpTimer.expired() && udpTimer.expired());
             loop.quit();
         }}, o);
    const auto weak = kcp::detail::KcpTestAccess::state(*s);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    kcpTimer = kcp::detail::KcpTestAccess::timerLifetime(*s);
    udpTimer = udp::detail::UdpPeerTestAccess::timerLifetime(
        kcp::detail::KcpTestAccess::udpServer(*s));
    GAMENET_TEST_ASSERT(!kcpTimer.expired() && !udpTimer.expired());
    auto p = s->openSession(raw.address(), 208).session;
    GAMENET_TEST_ASSERT(p);
    if (accepted)
        GAMENET_TEST_ASSERT(p->send("pending").id && p->send("").id);
    watchdog = loop.runAfter(2s, [] { GAMENET_TEST_FAIL("KCP/UDP idle ordering did not settle"); });
    if (order == IdleOrder::KcpFirst) {
        kcp::detail::KcpTestAccess::advance(*s, 1h + 1ms);
        net::detail::EventLoopActiveBatchHarness::runFairRound(loop, {}, base::now());
        GAMENET_TEST_ASSERT(!p->isOpen()); // KCP has already committed IdleTimeout.
    } else if (order == IdleOrder::ExplicitCloseFirst)
        GAMENET_TEST_ASSERT(p->close() == kcp::KcpResult::Accepted);
    // Force the real UDP idle and lifecycle paths synchronously. No fake close
    // callback/native completion, or wall-clock race, selects the winning seal.
    auto& wire = kcp::detail::KcpTestAccess::udpServer(*s);
    udp::detail::UdpPeerTestAccess::expire(wire);
    udp::detail::UdpPeerTestAccess::visit(wire);
    GAMENET_TEST_ASSERT(!p->isOpen() && s->snapshot().udp.idleClosed == 1);
    loop.loop();
    GAMENET_TEST_ASSERT(completed == accepted && closed == 1 && stopped == 1 &&
                        !loop.attachedLifecycleNodeCount());
    s.reset();
    GAMENET_TEST_ASSERT(weak.expired());
}
void cancel(bool saturated) {
    net::EventLoopOptions lo;
    lo.maxPendingFunctors = 8;
    lo.maxFunctorsPerIteration = 8;
    lo.reservedPendingFunctors = 0;
    net::EventLoop loop(lo);
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.maxSessions = 2;
    o.maxSessionsPerTurn = 1;
    o.maxMessagesPerSessionTurn = 1;
    o.maxPendingMessagesPerSession = 2;
    o.maxPendingMessages = 2;
    unsigned complete = 0, closed = 0;
    bool stopped = false;
    std::set<std::uint64_t> ids;
    std::shared_ptr<kcp::KcpTransport> s;
    s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Cancelled &&
                                     ids.insert(c.id).second);
                 ++complete;
                 ledger(s->snapshot());
                 (void)s->stop();
             },
         .closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::Normal);
                 ++closed;
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 stopped = true;
                 s.reset();
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    auto p = s->openSession(raw.address(), 7).session;
    GAMENET_TEST_ASSERT(p);
    GAMENET_TEST_ASSERT(s->openSession(raw.address(), 7).result == kcp::KcpResult::InvalidArgument);
    GAMENET_TEST_ASSERT(p->send("a").result == kcp::KcpResult::Accepted);
    GAMENET_TEST_ASSERT(p->send("").result == kcp::KcpResult::Accepted);
    auto rejected = p->send("full");
    GAMENET_TEST_ASSERT(rejected.result == kcp::KcpResult::Overloaded && !rejected.id);
    if (saturated)
        while (loop.executor().post([] {}) == net::PostResult::Accepted) {
        }
    std::thread closer([&] {
        GAMENET_TEST_ASSERT(p->send("foreign").result == kcp::KcpResult::WrongThread);
        GAMENET_TEST_ASSERT(p->requestClose() == DispatchResult::Accepted);
        GAMENET_TEST_ASSERT(!p->isOpen() && p->requestClose() == DispatchResult::EndpointClosed);
        (void)s->stop();
    });
    closer.join();
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("KCP cancel timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped && complete == 2 && closed == 1 && !s);
    GAMENET_TEST_ASSERT(p->send("old").result == kcp::KcpResult::Stopped);
    GAMENET_TEST_ASSERT(!loop.attachedLifecycleNodeCount());
}
void timeout() {
    net::EventLoop loop;
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.sendTimeout = 80ms;
    unsigned complete = 0;
    bool closed = false, stopped = false;
    auto s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Failed);
                 ++complete;
             },
         .closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::Timeout);
                 closed = true;
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 stopped = true;
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    auto p = s->openSession(raw.address(), 9).session;
    GAMENET_TEST_ASSERT(p && p->send("lost").id);
    loop.runEvery(5ms, [&] {
        if (closed)
            (void)s->stop();
    });
    loop.runAfter(5s, [&] {
        auto x = s->snapshot();
        std::cerr << "deadline complete=" << complete << " closed=" << closed
                  << " phase=" << int(x.phase) << " peers=" << x.sessions << " udp=" << x.udp.peers
                  << " udpPhase=" << int(x.udp.phase) << " native=" << x.udp.socket.nativeOperations
                  << " posts=" << x.scheduledPosts << " pending=" << x.pendingMessages << std::endl;
        GAMENET_TEST_FAIL("KCP deadline timeout");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped && complete == 1);
}
void acknowledgedBeforeClose() {
    net::EventLoop loop;
    test::RawUdp raw;
    unsigned acked = 0, cancelled = 0, closed = 0;
    bool stopped = false, sealed = false;
    std::shared_ptr<kcp::KcpTransport> s;
    s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete =
             [&](auto, auto c) {
                 if (c.status == kcp::KcpSendStatus::Acknowledged)
                     ++acked;
                 else {
                     GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Cancelled);
                     ++cancelled;
                 }
                 s.reset(); // Last wrapper release during terminal callback; active lease must
                            // drain.
             },
         .closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::Normal);
                 ++closed;
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 stopped = true;
                 loop.quit();
             }});
    auto weak = kcp::detail::KcpTestAccess::state(*s);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    auto p = s->openSession(raw.address(), 200).session;
    GAMENET_TEST_ASSERT(p && p->send("").id && p->send("later").id);
    loop.runEvery(1ms, [&] {
        if (!s || sealed)
            return;
        char bytes[1200];
        const auto n = recv(raw.fd, bytes, sizeof(bytes), 0);
        if (n < 24)
            return;
        GAMENET_TEST_ASSERT(static_cast<unsigned char>(bytes[4]) == 81);
        auto ack = test::kcpPacket(200, 82, 0, 1);
        test::kcpWrite32(ack, 8, test::kcpRead32(bytes + 8));
        kcp::detail::KcpTestAccess::input(*s, p->identity(),
                                          ack); // ACK committed, no callback yet.
        GAMENET_TEST_ASSERT(!acked && !cancelled);
        GAMENET_TEST_ASSERT(p->close() == kcp::KcpResult::Accepted);
        sealed = true;
    });
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("KCP ACK/close/release timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(acked == 1 && cancelled == 1 && closed == 1 && stopped && weak.expired());
}
void schedulingRefusal() {
    net::EventLoopOptions lo;
    lo.maxPendingFunctors = 2;
    lo.maxFunctorsPerIteration = 2;
    lo.reservedPendingFunctors = 0;
    net::EventLoop loop(lo);
    test::RawUdp raw;
    unsigned failed = 0, closed = 0;
    auto s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Failed);
                 ++failed;
             },
         .closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::SchedulingRejected);
                 ++closed;
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 loop.quit();
             }});
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    auto p = s->openSession(raw.address(), 201).session;
    GAMENET_TEST_ASSERT(p && p->send("accepted").id);
    while (loop.executor().post([] {}) == net::PostResult::Accepted) {
    }
    kcp::detail::KcpTestAccess::advance(*s, 0ms);
    GAMENET_TEST_ASSERT(!p->isOpen());
    loop.loop();
    GAMENET_TEST_ASSERT(failed == 1 && closed == 1);
}
void idleAndHistory() {
    net::EventLoop loop;
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.maxSessions = 1;
    o.maxSessionsPerTurn = 1;
    o.maxSessionOpenings = 1;
    o.idleTimeout = 20ms;
    unsigned closed = 0;
    std::shared_ptr<kcp::KcpTransport> s;
    s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(reason == kcp::KcpCloseReason::IdleTimeout);
                 ++closed;
                 GAMENET_TEST_ASSERT(s->openSession(raw.address(), 202).result ==
                                     kcp::KcpResult::InvalidArgument);
                 GAMENET_TEST_ASSERT(s->openSession(raw.address(), 203).result ==
                                     kcp::KcpResult::Overloaded);
                 (void)s->stop();
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 GAMENET_TEST_ASSERT(snap.convHistory == 1);
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted &&
                        s->openSession(raw.address(), 202).session);
    kcp::detail::KcpTestAccess::advance(*s, 40ms);
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1);
}
void memoryAndOptions() {
    kcp::KcpOptions bad;
    bad.mtu = 1201;
    try {
        bad.validate();
        GAMENET_TEST_FAIL("invalid KCP MTU accepted");
    } catch (const std::invalid_argument&) {
    }
    net::EventLoop loop;
    test::RawUdp raw;
    kcp::KcpOptions o;
    o.maxProtocolBytes = 1;
    auto s =
        kcp::KcpTransport::create(&loop, net::InetAddress("127.0.0.1", 0),
                                  {.stopped =
                                       [&](auto snap) {
                                           ledger(snap);
                                           GAMENET_TEST_ASSERT(!snap.opened && !snap.protocolBytes);
                                           loop.quit();
                                       }},
                                  o);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    GAMENET_TEST_ASSERT(s->openSession(raw.address(), 204).result == kcp::KcpResult::Overloaded);
    (void)s->stop();
    loop.loop();
}
void aggregateBudget() {
    net::EventLoop loop;
    test::RawUdp a, b;
    kcp::KcpOptions o;
    o.maxSessions = 2;
    o.maxSessionsPerTurn = 1;
    o.maxMessageBytes = 1024;
    o.maxPendingBytesPerSession = 2048;
    o.maxPendingBytes = 2048;
    o.maxPendingMessagesPerSession = 2;
    o.maxPendingMessages = 3;
    unsigned terminals = 0;
    auto s = kcp::KcpTransport::create(
        &loop, net::InetAddress("127.0.0.1", 0),
        {.sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Cancelled);
                 ++terminals;
             },
         .stopped =
             [&](auto snap) {
                 ledger(snap);
                 GAMENET_TEST_ASSERT(snap.accepted == 3 && snap.cancelled == 3);
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::Accepted);
    auto p = s->openSession(a.address(), 205).session, q = s->openSession(b.address(), 206).session;
    GAMENET_TEST_ASSERT(p && q && p->send(std::string(1024, 'x')).id &&
                        p->send(std::string(1024, 'y')).id);
    GAMENET_TEST_ASSERT(q->send("x").result == kcp::KcpResult::Overloaded);
    GAMENET_TEST_ASSERT(q->send("").id && q->send("").result == kcp::KcpResult::Overloaded);
    auto snap = s->snapshot();
    GAMENET_TEST_ASSERT(snap.pendingBytes == 2048 && snap.pendingMessages == 3);
    (void)s->stop();
    loop.loop();
    GAMENET_TEST_ASSERT(terminals == 3);
}
void failedStart() {
    for (std::size_t capacity : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
        net::EventLoop loop({.maxLifecycleNodes = capacity});
        unsigned stopped = 0;
        auto s = kcp::KcpTransport::create(&loop, net::InetAddress("127.0.0.1", 0),
                                           {.stopped = [&](auto snap) {
                                               ledger(snap);
                                               ++stopped;
                                               loop.quit();
                                           }});
        const auto weak = kcp::detail::KcpTestAccess::state(*s);
        GAMENET_TEST_ASSERT(s->start() == kcp::KcpResult::OwnerUnavailable);
        if (capacity) {
            loop.runAfter(1s, [] { GAMENET_TEST_FAIL("KCP partial start leaked"); });
            loop.loop();
            GAMENET_TEST_ASSERT(stopped == 1);
        } else
            GAMENET_TEST_ASSERT(!stopped && s->snapshot().phase == udp::UdpPhase::Stopped);
        s.reset();
        GAMENET_TEST_ASSERT(weak.expired() && !loop.attachedLifecycleNodeCount());
    }
}
int main() {
    for (auto action : {CallbackClose::Stop, CallbackClose::ForeignStop, CallbackClose::Quit,
                        CallbackClose::ForeignQuit, CallbackClose::Release, CallbackClose::Throw,
                        CallbackClose::SessionClose})
        for (bool acknowledgment : {false, true})
            callbackCloseAtSendDeadline(action, acknowledgment);
    for (bool foreign : {false, true})
        for (bool saturated : {false, true})
            quitRetiresTimers(foreign, saturated);
    quitRetiresTimers(false, false, true);
    quitRetiresTimers(false, true, true);
    for (auto order : {IdleOrder::UdpFirst, IdleOrder::KcpFirst, IdleOrder::ExplicitCloseFirst})
        idleCloseInterleaving(order);
    std::cerr << "KCP failed start\n";
    failedStart();
    std::cerr << "KCP cancel\n";
    cancel(false);
    cancel(true);
    std::cerr << "KCP timeout\n";
    timeout();
    std::cerr << "KCP ACK/close/release\n";
    acknowledgedBeforeClose();
    std::cerr << "KCP scheduling\n";
    schedulingRefusal();
    std::cerr << "KCP idle/history\n";
    idleAndHistory();
    std::cerr << "KCP memory\n";
    memoryAndOptions();
    std::cerr << "KCP aggregate budget\n";
    aggregateBudget();
    std::cerr << "KCP state sequences\n";
    for (unsigned seed = 1; seed <= 32; ++seed) {
        std::array<std::uint8_t, 96> sequence{};
        auto x = seed;
        for (auto& v : sequence) {
            x = x * 1664525 + 1013904223;
            v = static_cast<std::uint8_t>(x >> 24);
        }
        test::runKcpSequence(sequence);
    }
}
