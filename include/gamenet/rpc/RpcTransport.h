// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

// RPC-C2：仅描述 RPC 的 owner、就绪状态和有界发送，不拥有套接字或调度线程。
// Intent: rpc.intent.md / rpc_tls.intent.md; ownership/thread affinity rules.
#include <string_view>
namespace gamenet::net { class EventLoop; }
namespace gamenet::rpc {
enum class RpcTransportState { NotReady, Ready, Closed };
class RpcTransport {
public:
    virtual ~RpcTransport() = default; // owner-only; ownerLoop outlives this object
    virtual net::EventLoop* ownerLoop() const noexcept = 0;
    // Except ownerLoop, operations belong to the owner. State never dispatches.
    virtual RpcTransportState state() const noexcept = 0;
    // True owns a copy before return; false owns nothing and queues no retry.
    // May synchronously report disconnect; never invokes RPC receive inline.
    virtual bool trySend(std::string_view wire) = 0;
    virtual void abort() = 0;
};
}  // namespace gamenet::rpc
