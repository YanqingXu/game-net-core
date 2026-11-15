// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "../../../src/kcp/KcpEngine.h"
#include "support/KcpSequence.h"
#include "support/TestAssert.h"
#include <deque>
#include <string>
using namespace gamenet::kcp;
using namespace gamenet::kcp::detail;
int output(void* p, std::string_view s) noexcept {
    try {
        static_cast<std::deque<std::string>*>(p)->emplace_back(s);
        return 0;
    } catch (...) {
        return -1;
    }
}
int main() {
    KcpOptions o;
    o.validate();
    std::deque<std::string> ab, ba;
    KcpEngine a(42, o, output, &ab), b(42, o, output, &ba);
    const std::string large(14000, 'x');
    auto empty = a.send("");
    auto big = a.send(large);
    GAMENET_TEST_ASSERT(empty && big);
    std::vector<std::string> received;
    bool dropped = false, ackDropped = false;
    for (std::uint32_t t = 0; t < 10000; t += 10) {
        GAMENET_TEST_ASSERT(a.update(t) && b.update(t));
        while (!ab.empty()) {
            auto s = std::move(ab.back());
            ab.pop_back(); // reverse order
            if (!dropped) {
                dropped = true;
                continue;
            }
            GAMENET_TEST_ASSERT(b.input(s) == InputResult::Accepted);
            // Duplicate data cannot duplicate application delivery.
            auto r = b.input(s);
            GAMENET_TEST_ASSERT(r == InputResult::Accepted || r == InputResult::AckBudget);
        }
        while (auto s = b.receive())
            received.emplace_back(*s);
        while (!ba.empty()) {
            auto s = std::move(ba.front());
            ba.pop_front();
            if (!ackDropped) {
                ackDropped = true;
                continue;
            }
            auto corrupt = s + 'z';
            const auto before = a.snapshot();
            GAMENET_TEST_ASSERT(a.input(corrupt) == InputResult::Invalid);
            GAMENET_TEST_ASSERT(a.snapshot().sendSegments == before.sendSegments);
            GAMENET_TEST_ASSERT(a.input(s) == InputResult::Accepted);
        }
        if (a.acknowledged(*big))
            break;
    }
    GAMENET_TEST_ASSERT(received == std::vector<std::string>({"", large}));
    GAMENET_TEST_ASSERT(a.acknowledged(*empty) && a.acknowledged(*big));
    GAMENET_TEST_ASSERT(a.snapshot().sendSegments == 0);
    GAMENET_TEST_ASSERT(a.snapshot().arenaUsed <= a.snapshot().arenaCapacity);
    // Count and segment pressure are independent of payload bytes.
    KcpEngine bounded(9, o, output, &ab);
    for (std::size_t n = 0; n < o.maxSendSegments; ++n)
        GAMENET_TEST_ASSERT(bounded.send(""));
    GAMENET_TEST_ASSERT(!bounded.send(""));
    GAMENET_TEST_ASSERT(!bounded.failed());
    GAMENET_TEST_ASSERT(!bounded.send(std::string(o.maxMessageBytes + 1, 'a')));
    bounded.exhaustNextAllocationForTesting();
    // New engine avoids rejection by the existing send-segment cap.
    KcpEngine fault(8, o, output, &ab);
    fault.exhaustNextAllocationForTesting();
    GAMENET_TEST_ASSERT(!fault.send("test") && fault.failed());
    GAMENET_TEST_ASSERT(fault.snapshot().arenaUsed == 0);
    // Whole-datagram validation rejects wrong conv, command and lengths before mutation.
    std::string malformed(24, '\0');
    for (std::size_t n = 0; n < malformed.size(); ++n) {
        auto v = malformed;
        v[n] = '\xff';
        GAMENET_TEST_ASSERT(b.input(v) == InputResult::Invalid);
    }
    KcpOptions tight = o;
    tight.maxAckEntries = 2;
    KcpEngine receiver(15, tight, output, &ba);
    auto packet = gamenet::test::kcpPacket(15, 81, 0, 0, "");
    GAMENET_TEST_ASSERT(receiver.input(packet) == InputResult::Accepted);
    GAMENET_TEST_ASSERT(receiver.input(packet) == InputResult::Accepted);
    const auto full = receiver.snapshot();
    GAMENET_TEST_ASSERT(full.ackEntries == 2 && receiver.input(packet) == InputResult::AckBudget);
    GAMENET_TEST_ASSERT(receiver.snapshot().arenaUsed == full.arenaUsed &&
                        receiver.snapshot().ackEntries == 2);
    GAMENET_TEST_ASSERT(receiver.update(0xfffffff0u));
    GAMENET_TEST_ASSERT(receiver.snapshot().ackEntries == 0 &&
                        receiver.input(packet) == InputResult::Accepted);
    GAMENET_TEST_ASSERT(receiver.update(40)); // 32-bit KCP timer wrap.
    KcpEngine inputFault(16, o, output, &ba);
    inputFault.exhaustNextAllocationForTesting();
    GAMENET_TEST_ASSERT(inputFault.input(gamenet::test::kcpPacket(16, 81, 0, 0, "x")) ==
                        InputResult::Failed);
    GAMENET_TEST_ASSERT(inputFault.failed() && inputFault.snapshot().arenaUsed == 0);
    KcpEngine chain(17, o, output, &ba);
    auto first = gamenet::test::kcpPacket(17, 81, 0, 0, "x");
    first[5] = 1;
    auto second = gamenet::test::kcpPacket(17, 81, 1, 0, "y");
    second[5] = 1;
    GAMENET_TEST_ASSERT(chain.input(first) == InputResult::Accepted &&
                        chain.input(second) == InputResult::Accepted);
    GAMENET_TEST_ASSERT(!chain.receive() && chain.failed());
    KcpEngine staleRtt(18, o, output, &ba);
    GAMENET_TEST_ASSERT(staleRtt.update(0x70000000));
    GAMENET_TEST_ASSERT(staleRtt.input(gamenet::test::kcpPacket(18, 82, 0, 0)) ==
                        InputResult::Invalid);
    GAMENET_TEST_ASSERT(!staleRtt.failed());
}
