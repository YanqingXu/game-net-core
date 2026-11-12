// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/WebSocketStateSequence.h"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    gamenet::test::runWebSocketStateSequence(std::span(data,size));return 0;
}
