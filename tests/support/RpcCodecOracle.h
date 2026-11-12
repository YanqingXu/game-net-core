// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/rpc/RpcCodec.h"
#include "support/TestAssert.h"
#include <cstdint>
#include <string_view>

namespace gamenet::test {
// Independent acceptance predicate: no production codec/framer helpers.
inline bool rpcWireValid(std::string_view bytes, std::size_t limit) {
    if (bytes.size() < 24 || bytes.size() - 24 > limit) return false;
    constexpr unsigned char prefix[]{'G', 'N', 'R', 'P', 1};
    for (unsigned i = 0; i < 5; ++i)
        if (static_cast<unsigned char>(bytes[i]) != prefix[i]) return false;
    if (bytes[6] || bytes[7]) return false;
    bool id = false, method = false, timeout = false;
    for (unsigned i = 8; i < 16; ++i) id |= bytes[i] != 0;
    for (unsigned i = 16; i < 20; ++i) method |= bytes[i] != 0;
    for (unsigned i = 20; i < 24; ++i) timeout |= bytes[i] != 0;
    return id && ((bytes[5] == 1 && method && timeout) ||
        ((bytes[5] == 2 || bytes[5] == 3) && !method && !timeout));
}
inline void verifyRpcCodec(std::string_view input, std::size_t limit) {
    const auto decoded = rpc::RpcCodec::decode(input, limit);
    GAMENET_TEST_ASSERT(decoded.has_value() == rpcWireValid(input, limit));
    if (decoded) {
        const auto encoded = rpc::RpcCodec::encode(*decoded, limit);
        GAMENET_TEST_ASSERT(encoded && *encoded == input);
        if (!decoded->payload.empty())
            GAMENET_TEST_ASSERT(!rpc::RpcCodec::encode(*decoded, decoded->payload.size() - 1));
    }
    rpc::RpcMessage message;
    message.kind = static_cast<rpc::RpcMessageKind>(input.empty() ? 0 :
        static_cast<unsigned char>(input[0]));
    message.requestId = input.size() > 1 ? static_cast<unsigned char>(input[1]) : 0;
    message.method = input.size() > 2 ? static_cast<unsigned char>(input[2]) : 0;
    message.timeoutMs = input.size() > 3 ? static_cast<unsigned char>(input[3]) : 0;
    message.payload = std::string(input.substr(0, limit + (input.size() > limit ? 1 : 0)));
    const auto encoded = rpc::RpcCodec::encode(message, limit);
    const bool valid = message.requestId && message.payload.size() <= limit &&
        ((message.kind == rpc::RpcMessageKind::Request && message.method && message.timeoutMs) ||
        ((message.kind == rpc::RpcMessageKind::Response || message.kind == rpc::RpcMessageKind::Error) &&
         !message.method && !message.timeoutMs));
    GAMENET_TEST_ASSERT(encoded.has_value() == valid);
    if (encoded) {
        GAMENET_TEST_ASSERT(rpcWireValid(*encoded, limit));
        const auto result = rpc::RpcCodec::decode(*encoded, limit);
        GAMENET_TEST_ASSERT(result && result->kind == message.kind && result->requestId == message.requestId &&
            result->method == message.method && result->timeoutMs == message.timeoutMs && result->payload == message.payload);
    }
}
} // namespace gamenet::test
