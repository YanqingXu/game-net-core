// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gamenet::rpc {

enum class RpcMessageKind : std::uint8_t { Request = 1, Response = 2, Error = 3 };
struct RpcMessage {
    RpcMessageKind kind{RpcMessageKind::Request};
    std::uint64_t requestId{};
    std::uint32_t method{};
    std::uint32_t timeoutMs{};
    std::string payload;
};

// Encodes a PacketFramer body, without the outer four-byte length prefix.
// Invalid fields/size return nullopt. No reactor or thread affinity.
class RpcCodec {
public:
    static constexpr std::size_t kHeaderBytes = 24;
    static constexpr std::size_t kDefaultMaxPayloadBytes = 64 * 1024;
    static std::optional<std::string> encode(
        const RpcMessage& message, std::size_t maxPayloadBytes = kDefaultMaxPayloadBytes);
    static std::optional<RpcMessage> decode(
        std::string_view body, std::size_t maxPayloadBytes = kDefaultMaxPayloadBytes);
};

}  // namespace gamenet::rpc
