// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/udp/UdpSocket.h"
#include "gamenet/core/net/EventLoop.h"
#include "support/RawUdp.h"
#include "support/TestAssert.h"
#include "../../../src/udp/UdpTestAccess.h"
#include <set>
#include <thread>
using namespace gamenet;
using namespace std::chrono_literals;
static void generation(unsigned id) {
    net::EventLoop loop;
    test::RawUdp peer(id % 2);
    unsigned hot = 0, cold = 0, ticks = 0, done = 0, closed = 0, stage = 0, rejected = 0;
    bool interrupted = false;
    std::set<std::uint64_t> completions;
    std::shared_ptr<udp::UdpSocket> a, b;
    udp::UdpOptions o{.maxDatagramBytes = 64,
                      .maxPendingDatagrams = 4,
                      .maxPendingBytes = 128,
                      .maxReceiveDatagramsPerBatch = 1,
                      .maxReceiveBytesPerBatch = 64,
                      .maxSendDatagramsPerBatch = 1,
                      .maxSendBytesPerBatch = 64};
    const auto local = net::InetAddress(id % 2 ? "::1" : "127.0.0.1", 0);
    auto stopped = [&](auto s) {
        test::verifyUdpLedger(s);
        GAMENET_TEST_ASSERT(s.maxReceiveBatchDatagrams <= 1 && s.maxSendBatchDatagrams <= 1 &&
                            s.maxReceiveBatchBytes <= 64 && s.maxSendBatchBytes <= 64);
        if (++closed == 2)
            loop.quit();
    };
    a = udp::UdpSocket::create(&loop, local,
                               {.datagram =
                                    [&, id](auto&, auto bytes, bool truncated) {
                                        GAMENET_TEST_ASSERT(!closed && !truncated &&
                                                            bytes == "hot-" + std::to_string(id));
                                        ++hot;
                                    },
                                .sendComplete =
                                    [&](auto c) {
                                        GAMENET_TEST_ASSERT(c.status != udp::UdpSendStatus::Error &&
                                                            completions.insert(c.id).second);
                                        ++done;
                                    },
                                .stopped = stopped},
                               o);
    b = udp::UdpSocket::create(&loop, local,
                               {.datagram =
                                    [&, id](auto&, auto bytes, bool truncated) {
                                        GAMENET_TEST_ASSERT(bytes == "cold-" + std::to_string(id) &&
                                                            !truncated && ++cold == 1 && hot <= 2);
                                    },
                                .stopped = stopped},
                               o);
    const auto oldA = udp::detail::UdpTestAccess::state(*a),
               oldB = udp::detail::UdpTestAccess::state(*b);
    GAMENET_TEST_ASSERT(a->start() == udp::UdpResult::Accepted &&
                        b->start() == udp::UdpResult::Accepted);
    for (unsigned n = 0; n < 64; ++n)
        peer.send(a->localAddress(), "hot-" + std::to_string(id));
    peer.send(b->localAddress(), "cold-" + std::to_string(id));
    for (unsigned n = 0; n < 64; ++n) {
        const auto r = a->sendTo(peer.address(), std::string(32, 'b'));
        if (n < 4)
            GAMENET_TEST_ASSERT(r.result == udp::UdpResult::Accepted);
        else {
            GAMENET_TEST_ASSERT(r.result == udp::UdpResult::Overloaded && !r.id);
            ++rejected;
        }
    }
    loop.runAfter(0ns, [&] {
        GAMENET_TEST_ASSERT(hot <= 2);
        ++ticks;
    });
    loop.runEvery(1ms, [&] {
        if (a)
            test::verifyUdpLedger(a->snapshot());
        if (stage == 0 && done == 4 && hot == 64 && cold == 1 && ticks == 1) {
            GAMENET_TEST_ASSERT(a->sendTo(peer.address(), "recovered").result ==
                                udp::UdpResult::Accepted);
            stage = 1;
        } else if (stage == 1 && done == 5) {
            stage = 2;
            for (unsigned n = 0; n < 4; ++n)
                GAMENET_TEST_ASSERT(a->sendTo(peer.address(), std::string(32, 'c')).result ==
                                    udp::UdpResult::Accepted);
        }
    });
    loop.setEventLoopMetricCallback([&](const auto& sample) {
        if (stage != 2 || interrupted || !a ||
            sample.event != net::EventLoopMetricEvent::LifecycleNodesDrained)
            return;
        const auto s = a->snapshot();
#ifdef _WIN32
        if (s.nativeOperations != 2)
            return; // real read AND write, queued or pending in kernel
#else
        if (!s.pendingDatagrams)
            return;
#endif
        interrupted = true;
        stage = 3;
        if (id % 3 == 0)
            loop.quit();
        else if (id % 3 == 1) {
            (void)a->stop();
            (void)b->stop();
            a.reset();
            b.reset();
        } else {
            auto aa = a, bb = b;
            std::thread worker([aa, bb] {
                (void)aa->stop();
                (void)bb->stop();
            });
            worker.join();
        }
    });
    loop.runAfter(10s, [id] {
        (void)id;
        GAMENET_TEST_FAIL("UDP pressure/fairness/restart timeout");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(interrupted && rejected == 60 && done == 9 && closed == 2 && hot == 64 &&
                        cold == 1 && ticks == 1);
    if (a)
        test::verifyUdpLedger(a->snapshot());
    if (b)
        test::verifyUdpLedger(b->snapshot());
    a.reset();
    b.reset();
    GAMENET_TEST_ASSERT(oldA.expired() && oldB.expired() && !loop.attachedLifecycleNodeCount());
}
int main() {
    for (unsigned id = 0; id < 16; ++id)
        generation(id);
}
