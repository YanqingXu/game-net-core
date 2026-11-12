// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc/RpcCodec.h"
#include "support/RpcCodecOracle.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

int main() {
    using namespace gamenet::rpc;
    const RpcMessage request{RpcMessageKind::Request, 0x0102030405060708ULL,
        0x11223344, 1234, std::string("a\0b", 3)};
    const auto body = RpcCodec::encode(request);
    GAMENET_TEST_ASSERT(body && body->size() == 27);
    for (std::size_t n = 0; n <= body->size(); ++n)
        for (const auto limit : {std::size_t(0), std::size_t(2), std::size_t(3), std::size_t(64)})
            gamenet::test::verifyRpcCodec(std::string_view(*body).substr(0, n), limit);
    for (std::size_t n = 0; n < body->size(); ++n) for (unsigned byte = 0; byte < 256; ++byte) {
        auto mutated = *body; mutated[n] = static_cast<char>(byte);
        gamenet::test::verifyRpcCodec(mutated, 3);
    }
    GAMENET_TEST_ASSERT(body->substr(0, 4) == "GNRP");
    GAMENET_TEST_ASSERT(static_cast<unsigned char>((*body)[8]) == 1);
    GAMENET_TEST_ASSERT(static_cast<unsigned char>((*body)[15]) == 8);
    const auto decoded = RpcCodec::decode(*body);
    GAMENET_TEST_ASSERT(decoded && decoded->requestId == request.requestId &&
        decoded->method == request.method && decoded->timeoutMs == 1234 &&
        decoded->payload == request.payload);
    gamenet::protocol::PacketFramer framer;
    const auto wire = framer.encode(*body);
    for (std::size_t n = 0; n < wire->size(); ++n) {
        auto result = framer.push(std::string_view(*wire).substr(n, 1));
        GAMENET_TEST_ASSERT(result.frames.size() == (n + 1 == wire->size() ? 1 : 0));
    }
    for (const auto offset : {0, 4, 5, 6, 7}) {
        auto invalid = *body;
        invalid[offset] = static_cast<char>(255);
        GAMENET_TEST_ASSERT(!RpcCodec::decode(invalid));
    }
    for (std::size_t n = 0; n < RpcCodec::kHeaderBytes; ++n)
        GAMENET_TEST_ASSERT(!RpcCodec::decode(std::string_view(*body).substr(0, n)));
    GAMENET_TEST_ASSERT(!RpcCodec::decode(*body, 2));
    GAMENET_TEST_ASSERT(!RpcCodec::encode(request, 2));
    GAMENET_TEST_ASSERT(!RpcCodec::encode({RpcMessageKind::Request, 0, 1, 1, ""}));
    GAMENET_TEST_ASSERT(!RpcCodec::encode({RpcMessageKind::Request, 1, 0, 1, ""}));
    GAMENET_TEST_ASSERT(!RpcCodec::encode({RpcMessageKind::Request, 1, 1, 0, ""}));
    for (const auto kind : {RpcMessageKind::Response, RpcMessageKind::Error}) {
        GAMENET_TEST_ASSERT(RpcCodec::encode({kind, 1, 0, 0, ""}));
        GAMENET_TEST_ASSERT(!RpcCodec::encode({kind, 1, 1, 0, ""}));
    }
}
