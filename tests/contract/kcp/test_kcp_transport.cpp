// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp/KcpTransport.h"
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
                            !s.udp.socket.nativeOperations);
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
int main() {
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
