// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/rpc/RpcTransport.h>
#include <gamenet/rpc/RpcChannel.h>
#include <stdexcept>
int main() {
    try { (void)gamenet::rpc::RpcChannel::createWithTransport({}); }
    catch (const std::invalid_argument&) { return 0; }
    return 1;
}
