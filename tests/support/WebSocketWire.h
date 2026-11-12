// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace gamenet::test {
inline const std::string websocketUpgrade = "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
// Independent client encoder: no production frame/SHA helpers.
inline std::string websocketClientFrame(unsigned opcode, std::string_view payload, bool fin = true, bool masked = true) {
    std::string wire; wire.push_back(static_cast<char>((fin ? 128 : 0) | opcode));
    auto byte = [&](unsigned value) { wire.push_back(static_cast<char>(value)); };
    const unsigned mask = masked ? 128 : 0;
    if (payload.size() < 126) byte(mask | static_cast<unsigned>(payload.size()));
    else if (payload.size() <= 65535) { byte(mask | 126); byte(static_cast<unsigned>(payload.size() >> 8)); byte(static_cast<unsigned>(payload.size())); }
    else { byte(mask | 127); for (int n = 7; n >= 0; --n) byte(static_cast<unsigned>(std::uint64_t(payload.size()) >> (n * 8))); }
    constexpr unsigned key[]{0x37, 0xfa, 0x21, 0x3d};
    if (masked) for (const auto value : key) byte(value);
    for (std::size_t n = 0; n < payload.size(); ++n) byte(static_cast<unsigned char>(payload[n]) ^ (masked ? key[n % 4] : 0));
    return wire;
}
} // namespace gamenet::test
