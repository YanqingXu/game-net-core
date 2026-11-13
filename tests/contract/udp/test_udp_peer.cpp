// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/udp/UdpPeer.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include "support/UdpPeerSequence.h"
#include <set>
#include <thread>
using namespace gamenet;
using namespace std::chrono_literals;
static void ledger(const udp::UdpPeerSnapshot& s) {
    GAMENET_TEST_ASSERT(s.acceptedDatagrams == s.sentDatagrams + s.cancelledDatagrams +
                                                   s.failedDatagrams + s.pendingDatagrams);
    GAMENET_TEST_ASSERT(s.acceptedBytes ==
                        s.sentBytes + s.cancelledBytes + s.failedBytes + s.pendingBytes);
    GAMENET_TEST_ASSERT(s.opened == s.closed + s.peers);
}
static void bounded(bool v6) {
    net::EventLoop loop;
    test::RawUdp a(v6), b(v6);
    udp::UdpPeerOptions o;
    o.maxPeers = 2;
    o.maxPendingDatagramsPerPeer = 2;
    o.maxPendingBytesPerPeer = 64;
    o.socket.maxDatagramBytes = 32;
    o.socket.maxPendingDatagrams = 3;
    o.socket.maxPendingBytes = 64;
    o.maxPeersPerTurn = 1;
    o.maxCompletionsPerTurn = 1;
    unsigned done = 0, closed = 0, received = 0;
    std::set<std::uint64_t> ids;
    std::shared_ptr<udp::UdpPeerServer> server;
    server = udp::UdpPeerServer::create(
        &loop, net::InetAddress(v6 ? "::1" : "127.0.0.1", 0),
        {.datagram =
             [&](auto peer, auto bytes, bool truncated) {
                 GAMENET_TEST_ASSERT(!truncated && bytes == "probe" && peer->isOpen());
                 ++received;
             },
         .sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(ids.insert(c.id).second);
                 ++done;
                 ledger(server->snapshot());
             },
         .closed = [&](auto, auto) { ++closed; },
         .stopped =
             [&](auto s) {
                 ledger(s);
                 GAMENET_TEST_ASSERT(!s.peers && !s.pendingBytes && !s.pendingDatagrams &&
                                     !s.scheduledPosts && !s.socket.nativeOperations);
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::Accepted);
    auto p = server->openPeer(a.address()).peer, q = server->openPeer(b.address()).peer;
    GAMENET_TEST_ASSERT(p && q && p->id() != q->id());
    GAMENET_TEST_ASSERT(server->openPeer(a.address()).result == udp::UdpResult::InvalidArgument);
    GAMENET_TEST_ASSERT(server->openPeer(net::InetAddress(v6 ? "::1" : "127.0.0.1", 9)).result ==
                        udp::UdpResult::Overloaded);
    GAMENET_TEST_ASSERT(p->sendDatagram(std::string(32, 'a')).result == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(p->sendDatagram(std::string(32, 'b')).result == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(p->sendDatagram("").result == udp::UdpResult::Overloaded);
    GAMENET_TEST_ASSERT(q->sendDatagram("").result == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(q->sendDatagram("").result == udp::UdpResult::Overloaded);
    std::thread foreign([&] {
        GAMENET_TEST_ASSERT(p->send("x") == transport::EndpointResult::WrongThread);
        GAMENET_TEST_ASSERT(server->openPeer(a.address()).result == udp::UdpResult::WrongThread);
        GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::WrongThread);
    });
    foreign.join();
    a.send(server->localAddress(), "probe");
    loop.runEvery(1ms, [&] {
        ledger(server->snapshot());
        if (done == 3 && received == 1)
            (void)server->stop();
    });
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP peer bounded timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(done == 3 && closed == 2 && !p->isOpen() &&
                        p->send("x") == transport::EndpointResult::Closed);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
}
static void saturatedClose() {
    net::EventLoopOptions lo;
    lo.maxPendingFunctors = 2;
    lo.maxFunctorsPerIteration = 2;
    lo.reservedPendingFunctors = 0;
    net::EventLoop loop(lo);
    test::RawUdp raw;
    unsigned complete = 0, closed = 0;
    bool stopped = false;
    auto s = udp::UdpPeerServer::create(&loop, net::InetAddress(0, true),
                                        {.sendComplete = [&](auto, auto) { ++complete; },
                                         .closed = [&](auto, auto) { ++closed; },
                                         .stopped =
                                             [&](auto snap) {
                                                 test::verifyPeerLedger(snap);
                                                 stopped = true;
                                                 loop.quit();
                                             }});
    GAMENET_TEST_ASSERT(s->start() == udp::UdpResult::Accepted);
    auto p = s->openPeer(raw.address()).peer;
    for (unsigned n = 0; n < 2; ++n)
        GAMENET_TEST_ASSERT(loop.executor().post([] {}) == net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(p->sendDatagram("rollback").result == udp::UdpResult::Overloaded);
    GAMENET_TEST_ASSERT(!s->snapshot().pendingBytes && !s->snapshot().acceptedDatagrams);
    std::thread closer([&] {
        GAMENET_TEST_ASSERT(p->requestClose(transport::CloseReason::Replaced) ==
                            DispatchResult::Accepted);
        GAMENET_TEST_ASSERT(p->requestClose(transport::CloseReason::Normal) ==
                            DispatchResult::EndpointClosed);
        (void)s->stop();
    });
    closer.join();
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("saturated peer close timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(stopped && closed == 1 && !complete);
}
static void identityAndIdle() {
    net::EventLoop loop;
    test::RawUdp raw;
    udp::UdpPeerOptions o;
    o.maxPeers = 1;
    o.idleTimeout = 10ms;
    unsigned closed = 0;
    std::shared_ptr<udp::UdpPeer> old, current;
    std::shared_ptr<udp::UdpPeerServer> s;
    s = udp::UdpPeerServer::create(
        &loop, net::InetAddress(0, true),
        {.closed =
             [&](auto identity, auto reason) {
                 ++closed;
                 if (closed == 1) {
                     GAMENET_TEST_ASSERT(identity == old->identity() &&
                                         reason == transport::CloseReason::Replaced);
                     current = s->openPeer(raw.address()).peer;
                     GAMENET_TEST_ASSERT(current && current->identity().slot == identity.slot &&
                                         current->identity().generation ==
                                             identity.generation + 1 &&
                                         current->id() != old->id());
                     GAMENET_TEST_ASSERT(old->send("old") == transport::EndpointResult::Closed);
                     udp::detail::UdpPeerTestAccess::replay(*s, 9999);
                 } else {
                     GAMENET_TEST_ASSERT(closed == 2 &&
                                         reason == transport::CloseReason::IdleTimeout);
                     (void)s->stop();
                 }
             },
         .stopped =
             [&](auto snap) {
                 test::verifyPeerLedger(snap);
                 GAMENET_TEST_ASSERT(snap.idleClosed == 1 && snap.ignoredCompletions == 1);
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == udp::UdpResult::Accepted);
    old = s->openPeer(raw.address()).peer;
    GAMENET_TEST_ASSERT(old->close(transport::CloseReason::Replaced) ==
                        transport::EndpointResult::Accepted);
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("identity/idle timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 2);
}
static void releaseAndException() {
    net::EventLoop loop;
    test::RawUdp raw;
    unsigned done = 0, closed = 0, stopped = 0;
    std::shared_ptr<udp::UdpPeerServer> s;
    s = udp::UdpPeerServer::create(&loop, net::InetAddress(0, true),
                                   {.datagram =
                                        [&](auto p, auto, bool) {
                                            GAMENET_TEST_ASSERT(p->sendDatagram("owned").result ==
                                                                udp::UdpResult::Accepted);
                                            s.reset();
                                            throw std::runtime_error("release in callback");
                                        },
                                    .sendComplete = [&](auto, auto) { ++done; },
                                    .closed =
                                        [&](auto, auto) {
                                            ++closed;
                                            throw std::runtime_error("closed callback");
                                        },
                                    .stopped =
                                        [&](auto snap) {
                                            test::verifyPeerLedger(snap);
                                            GAMENET_TEST_ASSERT(snap.callbackExceptions == 2);
                                            ++stopped;
                                            loop.quit();
                                        }});
    const auto weak = udp::detail::UdpPeerTestAccess::state(*s);
    GAMENET_TEST_ASSERT(s->start() == udp::UdpResult::Accepted);
    auto p = s->openPeer(raw.address()).peer;
    raw.send(s->localAddress(), "release");
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("peer callback release timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(!s && weak.expired() && done == 1 && closed == 1 && stopped == 1 &&
                        !p->isOpen());
}
static void lateNativeAndQuit() {
    net::EventLoop loop;
    test::RawUdp raw;
    udp::UdpPeerOptions o;
    o.maxPeers = 2;
    o.maxPeersPerTurn = 1;
    o.maxCompletionsPerTurn = 1;
    unsigned done = 0, closed = 0, stopped = 0;
    std::uint64_t oldReceipt = 0;
    std::shared_ptr<udp::UdpPeerServer> s;
    std::shared_ptr<udp::UdpPeer> old, current;
    s = udp::UdpPeerServer::create(
        &loop, net::InetAddress(0, true),
        {.sendComplete =
             [&](auto identity, auto) {
                 ++done;
                 if (identity == current->identity())
                     loop.quit();
             },
         .closed =
             [&](auto identity, auto) {
                 ++closed;
                 if (identity == old->identity()) {
                     GAMENET_TEST_ASSERT(current->isOpen());
                     const auto before = s->snapshot();
                     udp::detail::UdpPeerTestAccess::replay(*s, oldReceipt);
                     const auto after = s->snapshot();
                     GAMENET_TEST_ASSERT(current->isOpen() &&
                                         before.pendingBytes == after.pendingBytes &&
                                         before.pendingDatagrams == after.pendingDatagrams &&
                                         after.ignoredCompletions == 1);
                 }
             },
         .stopped =
             [&](auto snap) {
                 test::verifyPeerLedger(snap);
                 ++stopped;
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == udp::UdpResult::Accepted);
    old = s->openPeer(raw.address()).peer;
    GAMENET_TEST_ASSERT(old->sendDatagram("old-native").result == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(loop.executor().post([&] {
        oldReceipt = udp::detail::UdpPeerTestAccess::nativeId(*s);
        GAMENET_TEST_ASSERT(oldReceipt);
        GAMENET_TEST_ASSERT(old->close(transport::CloseReason::Replaced) ==
                            transport::EndpointResult::Accepted);
        current = s->openPeer(raw.address()).peer;
        GAMENET_TEST_ASSERT(current);
        GAMENET_TEST_ASSERT(current->sendDatagram("new-native").result == udp::UdpResult::Accepted);
    }) == net::PostResult::Accepted);
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("late native/quit timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(done == 2 && closed == 2 && stopped == 1 &&
                        loop.attachedLifecycleNodeCount() == 0);
}
static void acceptedForeignClose() {
    net::EventLoopOptions lo;
    lo.maxPendingFunctors = 2;
    lo.maxFunctorsPerIteration = 2;
    lo.reservedPendingFunctors = 0;
    net::EventLoop loop(lo);
    test::RawUdp raw;
    unsigned done = 0, closed = 0;
    std::set<std::uint64_t> ids;
    udp::UdpPeerOptions o;
    o.maxPeers = 3;
    o.maxPeersPerTurn = 1;
    o.maxCompletionsPerTurn = 1;
    auto s = udp::UdpPeerServer::create(
        &loop, net::InetAddress(0, true),
        {.sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Cancelled &&
                                     ids.insert(c.id).second);
                 ++done;
             },
         .closed =
             [&](auto, auto reason) {
                 GAMENET_TEST_ASSERT(done == 4 && reason == transport::CloseReason::Replaced);
                 ++closed;
             },
         .stopped =
             [&](auto snap) {
                 test::verifyPeerLedger(snap);
                 loop.quit();
             }},
        o);
    GAMENET_TEST_ASSERT(s->start() == udp::UdpResult::Accepted);
    auto p = s->openPeer(raw.address()).peer;
    for (unsigned n = 0; n < 4; ++n)
        GAMENET_TEST_ASSERT(p->sendDatagram("reserved").result == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(loop.executor().post([] {}) == net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(loop.executor().post([] {}) == net::PostResult::QueueFull);
    std::thread foreign([&] {
        GAMENET_TEST_ASSERT(p->requestClose(transport::CloseReason::Replaced) ==
                            DispatchResult::Accepted);
        (void)s->stop();
    });
    foreign.join();
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("accepted foreign close timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(done == 4 && closed == 1);
}
static void detachedHandleAndUnstarted() {
    std::shared_ptr<udp::UdpPeer> handle;
    {
        net::EventLoop loop;
        test::RawUdp raw;
        unsigned stopped = 0;
        auto server =
            udp::UdpPeerServer::create(&loop, net::InetAddress(0, true), {.stopped = [&](auto s) {
                test::verifyPeerLedger(s);
                ++stopped;
                loop.quit();
            }});
        GAMENET_TEST_ASSERT(server->openPeer(raw.address()).result == udp::UdpResult::Stopped);
        GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::Accepted &&
                            server->start() == udp::UdpResult::AlreadyStarted);
        handle = server->openPeer(raw.address()).peer;
        const auto weak = udp::detail::UdpPeerTestAccess::state(*server);
        (void)server->stop();
        loop.loop();
        server.reset();
        GAMENET_TEST_ASSERT(stopped == 1 && weak.expired());
    }
    GAMENET_TEST_ASSERT(!handle->isOpen() && !handle->ownerExecutor().available() &&
                        handle->send("after-loop") == transport::EndpointResult::Closed &&
                        handle->requestClose(transport::CloseReason::Normal) ==
                            DispatchResult::EndpointClosed);
    {
        net::EventLoop loop;
        unsigned calls = 0;
        auto s = udp::UdpPeerServer::create(&loop, net::InetAddress(0, true),
                                            {.stopped = [&](auto) { ++calls; }});
        GAMENET_TEST_ASSERT(s->stop() == udp::UdpResult::Stopped &&
                            s->start() == udp::UdpResult::Stopped && calls == 0 &&
                            loop.attachedLifecycleNodeCount() == 0);
    }
}
int main() {
    for (std::size_t capacity : {std::size_t(0), std::size_t(1)}) {
        net::EventLoop loop({.maxLifecycleNodes = capacity});
        unsigned stopped = 0;
        auto server = udp::UdpPeerServer::create(&loop, net::InetAddress(0, true),
                                                 {.stopped = [&](auto snap) {
                                                     test::verifyPeerLedger(snap);
                                                     ++stopped;
                                                     loop.quit();
                                                 }});
        GAMENET_TEST_ASSERT(server->start() == udp::UdpResult::OwnerUnavailable);
        if (capacity) {
            loop.runAfter(5s, [] { GAMENET_TEST_FAIL("failed start cleanup timeout"); });
            loop.loop();
            GAMENET_TEST_ASSERT(stopped == 1);
        } else
            GAMENET_TEST_ASSERT(!stopped && server->snapshot().phase == udp::UdpPhase::Stopped);
        GAMENET_TEST_ASSERT(!loop.attachedLifecycleNodeCount());
    }
    bounded(false);
    bounded(true);
    detachedHandleAndUnstarted();
    lateNativeAndQuit();
    acceptedForeignClose();
    saturatedClose();
    identityAndIdle();
    releaseAndException();
    for (unsigned seed = 0; seed < 32; ++seed) {
        std::array<std::uint8_t, 96> actions{};
        auto x = seed + 1;
        for (auto& a : actions) {
            x = x * 1664525u + 1013904223u;
            a = static_cast<std::uint8_t>(x >> 24);
        }
        test::runUdpPeerSequence(actions);
    }
}
