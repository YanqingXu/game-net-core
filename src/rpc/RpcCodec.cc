// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc/RpcCodec.h"

namespace gamenet::rpc {
namespace {
bool valid(const RpcMessage& message) {
    if (message.requestId == 0) return false;
    switch (message.kind) {
    case RpcMessageKind::Request: return message.method != 0 && message.timeoutMs != 0;
    case RpcMessageKind::Response:
    case RpcMessageKind::Error: return message.method == 0 && message.timeoutMs == 0;
    }
    return false;
}
void put(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned n = width; n != 0; --n)
        bytes.push_back(static_cast<char>((value >> ((n - 1) * 8)) & 255));
}
std::uint64_t get(std::string_view bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned n = 0; n < width; ++n)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + n]);
    return value;
}
}  // namespace

std::optional<std::string> RpcCodec::encode(const RpcMessage& message, std::size_t limit) {
    if (!valid(message) || message.payload.size() > limit) return std::nullopt;
    std::string bytes("GNRP");
    bytes.push_back(1);
    bytes.push_back(static_cast<char>(message.kind));
    put(bytes, 0, 2);
    put(bytes, message.requestId, 8);
    put(bytes, message.method, 4);
    put(bytes, message.timeoutMs, 4);
    bytes += message.payload;
    return bytes;
}

std::optional<RpcMessage> RpcCodec::decode(std::string_view bytes, std::size_t limit) {
    if (bytes.size() < kHeaderBytes || bytes.size() - kHeaderBytes > limit ||
        bytes.substr(0, 4) != "GNRP" || bytes[4] != 1 || bytes[6] != 0 || bytes[7] != 0)
        return std::nullopt;
    RpcMessage message{static_cast<RpcMessageKind>(static_cast<unsigned char>(bytes[5])),
        get(bytes, 8, 8), static_cast<std::uint32_t>(get(bytes, 16, 4)),
        static_cast<std::uint32_t>(get(bytes, 20, 4)), {}};
    if (!valid(message)) return std::nullopt;
    message.payload.assign(bytes.substr(kHeaderBytes));
    return message;
}
}  // namespace gamenet::rpc
