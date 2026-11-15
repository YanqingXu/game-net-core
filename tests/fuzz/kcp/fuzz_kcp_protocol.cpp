// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "../../../src/kcp/KcpEngine.h"
#include "support/KcpSequence.h"
#include "support/TestAssert.h"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace gamenet::kcp;
    using namespace gamenet::kcp::detail;
    KcpOptions o;
    o.mtu = 512;
    o.maxMessageBytes = 2048;
    o.maxSendSegments = 8;
    o.sendWindow = 4;
    o.maxAckEntries = 8;
    KcpEngine e(7, o, [](void*, std::string_view) noexcept { return -1; }, nullptr);
    (void)e.input({reinterpret_cast<const char*>(data), std::min<std::size_t>(size, 512)});
    for (std::size_t at = 0; at < std::min<std::size_t>(size, 512); at += 16) {
        const auto n = std::min<std::size_t>(size - at, 16);
        std::string bytes(reinterpret_cast<const char*>(data + at), n);
        if (data[at] & 1) {
            const auto command = 81 + ((data[at] >> 4) % 4);
            bytes = gamenet::test::kcpPacket(7, command, data[at] >> 2, 0,
                                             command == 81 ? std::string_view(bytes)
                                                           : std::string_view{});
        }
        if (data[at] & 2 && bytes.size() > 5)
            bytes[5] = static_cast<char>(data[at]);
        const auto before = e.snapshot();
        const auto result = e.input(bytes);
        const auto after = e.snapshot();
        if (result == InputResult::Invalid || result == InputResult::AckBudget)
            GAMENET_TEST_ASSERT(before.sendSegments == after.sendSegments &&
                                before.receiveSegments == after.receiveSegments &&
                                before.ackEntries == after.ackEntries &&
                                before.arenaUsed == after.arenaUsed);
        if (data[at] & 4)
            (void)e.send(std::string(data[at] % 128, 's'));
        if (data[at] & 8)
            (void)e.receive();
        (void)e.update(static_cast<std::uint32_t>(at * 20));
        auto s = e.snapshot();
        GAMENET_TEST_ASSERT(s.sendSegments <= 8 && s.receiveSegments <= 256 && s.ackEntries <= 8 &&
                            s.arenaUsed <= s.arenaCapacity);
        if (e.failed())
            break;
    }
    return 0;
}
