// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/rpc/RpcChannel.h>
#include <gamenet/core/net/EventLoop.h>
#include <stdexcept>
int verifyRpcInstallConsumer() {
    gamenet::net::EventLoop loop;
    const auto wire = gamenet::rpc::RpcCodec::encode(
        {gamenet::rpc::RpcMessageKind::Request, 1, 1, 1000, "installed"});
    if (!wire || !gamenet::rpc::RpcCodec::decode(*wire)) return 1;
    try { (void)gamenet::rpc::RpcChannel::create({}); }
    catch (const std::invalid_argument&) { return 0; }
    return 2;
}
