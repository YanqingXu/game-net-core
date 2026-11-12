// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/udp/UdpSocket.h"
#include "gamenet/core/net/EventLoop.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include "../../../src/udp/UdpTestAccess.h"
#include <set>
#include <barrier>
#include <mutex>
#include <vector>
#include <thread>
#include <stdexcept>
using namespace gamenet;
using namespace std::chrono_literals;
static void datagrams(bool ipv6) {
    net::EventLoop loop;
    test::RawUdp peer(ipv6);
    udp::UdpOptions o;
    o.maxDatagramBytes = 64;
    o.maxPendingDatagrams = 4;
    o.maxPendingBytes = 128;
    o.maxReceiveDatagramsPerBatch = 2;
    o.maxReceiveBytesPerBatch = 64;
    o.maxSendDatagramsPerBatch = 1;
    o.maxSendBytesPerBatch = 64;
    unsigned received = 0, completed = 0, closed = 0, phase = 0;
    std::set<std::uint64_t> ids;
    std::shared_ptr<udp::UdpSocket> socket;
    socket = udp::UdpSocket::create(
        &loop, net::InetAddress(ipv6 ? "::1" : "127.0.0.1", 0),
        {.datagram =
             [&](const auto& address, auto bytes, bool truncated) {
                 GAMENET_TEST_ASSERT(address.family() == socket->localAddress().family());
                 if (received == 0)
                     GAMENET_TEST_ASSERT(bytes.empty() && !truncated);
                 if (received == 1)
                     GAMENET_TEST_ASSERT(bytes == std::string(64, 'x') && !truncated);
                 if (received == 2)
                     GAMENET_TEST_ASSERT(bytes == std::string(64, 'y') && truncated);
                 if (received == 3)
                     GAMENET_TEST_ASSERT(bytes == "after" && !truncated);
                 GAMENET_TEST_ASSERT(++received <= 4);
             },
         .sendComplete =
             [&](auto c) {
                 GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Sent &&
                                     ids.insert(c.id).second);
                 ++completed;
                 test::verifyUdpLedger(socket->snapshot());
             },
         .stopped =
             [&](auto s) {
                 GAMENET_TEST_ASSERT(++closed == 1);
                 test::verifyUdpLedger(s);
                 loop.quit();
             }},
        o);
    const auto state = udp::detail::UdpTestAccess::state(*socket);
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted &&
                        socket->start() == udp::UdpResult::AlreadyStarted);
    peer.send(socket->localAddress(), "");
    peer.send(socket->localAddress(), std::string(64, 'x'));
    peer.send(socket->localAddress(), std::string(256, 'y'));
    peer.send(socket->localAddress(), "after");
    for (auto bytes : {std::string{}, std::string{}, std::string(64, 'a'), std::string(64, 'b')})
        GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), bytes).result ==
                            udp::UdpResult::Accepted);
    loop.runEvery(1ms, [&] {
        const auto s = socket->snapshot();
        test::verifyUdpLedger(s);
        if (phase == 0 && completed == 4) {
            phase = 1;
            GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), std::string(64, 'a')).result ==
                                udp::UdpResult::Accepted);
            GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), std::string(64, 'b')).result ==
                                udp::UdpResult::Accepted);
            GAMENET_TEST_ASSERT(socket->sendTo(socket->localAddress(), "c").result ==
                                udp::UdpResult::Overloaded);
        } else if (phase == 1 && completed == 6 && received == 4) {
            GAMENET_TEST_ASSERT(s.truncatedDatagrams == 1 && s.maxReceiveBatchDatagrams <= 2 &&
                                s.maxReceiveBatchBytes <= 64 && s.maxSendBatchDatagrams <= 1 &&
                                s.maxSendBatchBytes <= 64);
            (void)socket->stop();
        }
    });
    GAMENET_TEST_ASSERT(socket->sendTo(socket->localAddress(), "").result ==
                        udp::UdpResult::Overloaded);
    GAMENET_TEST_ASSERT(socket->sendTo(socket->localAddress(), std::string(65, 'x')).result ==
                        udp::UdpResult::InvalidArgument);
    GAMENET_TEST_ASSERT(
        socket->sendTo(net::InetAddress(ipv6 ? "127.0.0.1" : "::1", 9), "").result ==
        udp::UdpResult::InvalidArgument);
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP datagram contract timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1 && completed == 6 && received == 4 &&
                        loop.attachedLifecycleNodeCount() == 0);
    socket.reset();
    GAMENET_TEST_ASSERT(state.expired());
}
static void foreignStop() {
    net::EventLoop loop(
        {.maxPendingFunctors = 1, .reservedPendingFunctors = 0, .maxFunctorsPerIteration = 1});
    unsigned completed = 0, closed = 0;
    auto socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.datagram = [](auto&, auto, bool) { GAMENET_TEST_FAIL("callback after stop seal"); },
         .sendComplete =
             [&](auto c) {
                 GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Cancelled);
                 ++completed;
             },
         .stopped =
             [&](auto s) {
                 GAMENET_TEST_ASSERT(++closed == 1 && s.cancelledDatagrams == 4);
                 test::verifyUdpLedger(s);
                 loop.quit();
             }},
        {.maxDatagramBytes = 64, .maxPendingDatagrams = 4, .maxPendingBytes = 128});
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    GAMENET_TEST_ASSERT(loop.executor().post([] {}) == net::PostResult::Accepted);
    const auto address = socket->localAddress();
    std::thread worker([&] {
        GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::WrongThread);
        for (unsigned i = 0; i < 4; ++i)
            GAMENET_TEST_ASSERT(socket->sendTo(address, "").result == udp::UdpResult::Accepted);
        GAMENET_TEST_ASSERT(socket->sendTo(address, "").result == udp::UdpResult::Overloaded);
        GAMENET_TEST_ASSERT(socket->stop() == udp::UdpResult::Accepted);
        GAMENET_TEST_ASSERT(socket->sendTo(address, "").result == udp::UdpResult::Stopped);
    });
    worker.join();
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1 && completed == 4 && loop.attachedLifecycleNodeCount() == 0);
    test::verifyUdpLedger(socket->snapshot());
}
static void reentry(unsigned mode) {
    net::EventLoop loop;
    test::RawUdp peer;
    unsigned received = 0, closed = 0, completed = 0;
    udp::UdpSnapshot final;
    std::shared_ptr<udp::UdpSocket> socket;
    socket = udp::UdpSocket::create(&loop, net::InetAddress(0, true),
                                    {.datagram =
                                         [&](auto& address, auto, bool) {
                                             ++received;
                                             GAMENET_TEST_ASSERT(
                                                 socket->sendTo(address, "reply").result ==
                                                 udp::UdpResult::Accepted);
                                             if (mode == 0)
                                                 (void)socket->stop();
                                             if (mode == 1)
                                                 socket.reset();
                                             if (mode == 2)
                                                 throw std::runtime_error("injected");
                                             if (mode == 3)
                                                 loop.quit();
                                         },
                                     .sendComplete = [&](auto) { ++completed; },
                                     .stopped =
                                         [&](auto s) {
                                             final = s;
                                             test::verifyUdpLedger(s);
                                             GAMENET_TEST_ASSERT(++closed == 1);
                                             loop.quit();
                                         }});
    const auto state = udp::detail::UdpTestAccess::state(*socket);
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    peer.send(socket->localAddress(), "request");
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP reentry timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1 && received == 1 && completed == 1 &&
                        final.callbackExceptions == (mode == 2 ? 1u : 0u));
    socket.reset();
    GAMENET_TEST_ASSERT(state.expired() && loop.attachedLifecycleNodeCount() == 0);
}
static void sendReentry(unsigned mode) {
    net::EventLoop loop;
    test::RawUdp peer;
    unsigned done = 0, closed = 0;
    std::set<std::uint64_t> ids;
    std::shared_ptr<udp::UdpSocket> socket;
    socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.sendComplete =
             [&](auto c) {
                 GAMENET_TEST_ASSERT(ids.insert(c.id).second);
                 ++done;
                 if (c.status == udp::UdpSendStatus::Cancelled) {
                     GAMENET_TEST_ASSERT(mode == 1 || mode == 2);
                     return;
                 }
                 GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Sent);
                 if (mode == 0 && done == 1)
                     GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), "again").result ==
                                         udp::UdpResult::Accepted);
                 else if (mode == 1)
                     socket.reset();
                 else if (mode == 2)
                     throw std::runtime_error("send callback");
                 else
                     (void)socket->stop();
             },
         .stopped =
             [&](auto summary) {
                 test::verifyUdpLedger(summary);
                 GAMENET_TEST_ASSERT(++closed == 1);
                 loop.quit();
                 if (mode == 3)
                     throw std::runtime_error("stopped callback");
             }});
    auto state = udp::detail::UdpTestAccess::state(*socket);
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted &&
                        socket->sendTo(peer.address(), "first").result == udp::UdpResult::Accepted);
    if (mode == 1 || mode == 2)
        for (unsigned n = 0; n < 2; ++n)
            GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), "queued").result ==
                                udp::UdpResult::Accepted);
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP send reentry timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1 && done == (mode == 0                  ? 2u
                                                : (mode == 1 || mode == 2) ? 3u
                                                                           : 1u));
    if (socket)
        GAMENET_TEST_ASSERT(socket->snapshot().callbackExceptions ==
                            ((mode == 2 || mode == 3) ? 1u : 0u));
    socket.reset();
    GAMENET_TEST_ASSERT(state.expired() && !loop.attachedLifecycleNodeCount());
}
static void producers() {
    net::EventLoop loop;
    unsigned done = 0;
    std::set<std::uint64_t> admitted, terminal;
    std::mutex mutex;
    auto socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.sendComplete =
             [&](auto c) {
                 GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Cancelled);
                 GAMENET_TEST_ASSERT(terminal.insert(c.id).second);
                 ++done;
             },
         .stopped =
             [&](auto s) {
                 test::verifyUdpLedger(s);
                 loop.quit();
             }},
        {.maxDatagramBytes = 64, .maxPendingDatagrams = 8, .maxPendingBytes = 128});
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    const auto address = socket->localAddress();
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 4; ++n)
        threads.emplace_back([&] {
            for (unsigned i = 0; i < 100; ++i) {
                auto result = socket->sendTo(address, std::string(16, 'x'));
                if (result.result == udp::UdpResult::Accepted) {
                    std::lock_guard lock(mutex);
                    GAMENET_TEST_ASSERT(admitted.insert(result.id).second);
                } else
                    GAMENET_TEST_ASSERT(result.result == udp::UdpResult::Overloaded && !result.id);
            }
        });
    for (auto& thread : threads)
        thread.join();
    GAMENET_TEST_ASSERT(admitted.size() == 8);
    test::verifyUdpLedger(socket->snapshot());
    (void)socket->stop();
    loop.loop();
    GAMENET_TEST_ASSERT(done == 8 && admitted == terminal && !loop.attachedLifecycleNodeCount());
}
#ifdef _WIN32
static void submissionFailures(unsigned mode) {
    net::EventLoop loop;
    test::RawUdp peer;
    unsigned closed = 0, errors = 0, sent = 0;
    std::shared_ptr<udp::UdpSocket> socket;
    socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.sendComplete =
             [&](auto c) {
                 if (c.status == udp::UdpSendStatus::Error) {
                     ++errors;
                     GAMENET_TEST_ASSERT(c.nativeError == WSAENOBUFS);
                     GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), "recovered").result ==
                                         udp::UdpResult::Accepted);
                 } else {
                     GAMENET_TEST_ASSERT(c.status == udp::UdpSendStatus::Sent);
                     ++sent;
                     (void)socket->stop();
                 }
             },
         .stopped =
             [&](auto s) {
                 GAMENET_TEST_ASSERT(++closed == 1);
                 test::verifyUdpLedger(s);
                 loop.quit();
             }});
    if (mode == 0)
        udp::detail::UdpTestAccess::failNextRead(WSAENOBUFS);
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    if (mode == 1) {
        udp::detail::UdpTestAccess::failNextWrite(WSAENOBUFS);
        GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), "bad").result ==
                            udp::UdpResult::Accepted);
    }
    if (mode == 2) {
        udp::detail::UdpTestAccess::failNextPreparation();
        GAMENET_TEST_ASSERT(socket->sendTo(peer.address(), "bad").result ==
                            udp::UdpResult::Accepted);
    }
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP native rejection timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 1 && errors == (mode ? 1u : 0u) && sent == (mode ? 1u : 0u) &&
                        !loop.attachedLifecycleNodeCount());
}
#endif
static void startStopRaces() {
    for (unsigned n = 0; n < 32; ++n) {
        net::EventLoop loop;
        unsigned closed = 0;
        std::barrier start(2);
        auto socket =
            udp::UdpSocket::create(&loop, net::InetAddress(0, true), {.stopped = [&](auto s) {
                test::verifyUdpLedger(s);
                ++closed;
                loop.quit();
            }});
        auto state = udp::detail::UdpTestAccess::state(*socket);
        std::thread worker([&] {
            start.arrive_and_wait();
            (void)socket->stop();
        });
        start.arrive_and_wait();
        const auto result = socket->start();
        worker.join();
        if (result == udp::UdpResult::Accepted) {
            loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP start/stop race timeout"); });
            loop.loop();
            GAMENET_TEST_ASSERT(closed == 1);
        } else
            GAMENET_TEST_ASSERT(result == udp::UdpResult::Stopped && !closed);
        GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Stopped);
        test::verifyUdpLedger(socket->snapshot());
        socket.reset();
        GAMENET_TEST_ASSERT(state.expired() && !loop.attachedLifecycleNodeCount());
    }
}
static void datagramErrorRecovery() {
    net::EventLoop loop;
    test::RawUdp peer;
    unsigned received = 0, closed = 0;
    std::shared_ptr<udp::UdpSocket> socket;
    socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.datagram =
             [&](auto&, auto bytes, bool truncated) {
                 GAMENET_TEST_ASSERT(bytes == "after-error" && !truncated && ++received == 1);
                 (void)socket->stop();
             },
         .stopped =
             [&](auto summary) {
                 GAMENET_TEST_ASSERT(++closed == 1 && summary.receiveErrors == 1);
                 test::verifyUdpLedger(summary);
                 loop.quit();
             }});
#ifdef _WIN32
    udp::detail::UdpTestAccess::failNextRead(WSAECONNRESET);
#else
    udp::detail::UdpTestAccess::failNextRead(ECONNREFUSED);
#endif
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    peer.send(socket->localAddress(), "after-error");
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP per-datagram error recovery timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(received == 1 && closed == 1);
}
static void stopBeforeCallbackCommit() {
    net::EventLoop loop;
    test::RawUdp peer;
    unsigned closed = 0;
    struct Gate {
        std::barrier<> received{2}, sealed{2};
    } gate;
    auto socket = udp::UdpSocket::create(
        &loop, net::InetAddress(0, true),
        {.datagram = [](auto&, auto,
                        bool) { GAMENET_TEST_FAIL("datagram committed after foreign stop seal"); },
         .stopped =
             [&](auto summary) {
                 test::verifyUdpLedger(summary);
                 ++closed;
                 loop.quit();
             }});
    auto state = udp::detail::UdpTestAccess::state(*socket);
    udp::detail::UdpTestAccess::beforeDatagramCommit(
        +[](void* context) {
            auto& g = *static_cast<Gate*>(context);
            g.received.arrive_and_wait();
            g.sealed.arrive_and_wait();
        },
        &gate);
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::Accepted);
    std::thread worker([&] {
        gate.received.arrive_and_wait();
        GAMENET_TEST_ASSERT(socket->stop() == udp::UdpResult::Accepted);
        gate.sealed.arrive_and_wait();
    });
    peer.send(socket->localAddress(), "seal-before-commit");
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("UDP callback commitment race timeout"); });
    loop.loop();
    worker.join();
    GAMENET_TEST_ASSERT(closed == 1 && !loop.attachedLifecycleNodeCount());
    socket.reset();
    GAMENET_TEST_ASSERT(state.expired());
}
int main() {
    stopBeforeCallbackCommit();
    datagramErrorRecovery();
    datagrams(false);
    datagrams(true);
    foreignStop();
    producers();
    startStopRaces();
    for (unsigned mode = 0; mode < 4; ++mode) {
        reentry(mode);
        sendReentry(mode);
    }
#ifdef _WIN32
    for (unsigned mode = 0; mode < 3; ++mode)
        submissionFailures(mode);
#endif
}
