// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/RpcCodecOracle.h"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    gamenet::test::verifyRpcCodec(input, size ? data[0] : 0);
    gamenet::test::verifyRpcCodec(input, 4096);
    return 0;
}
