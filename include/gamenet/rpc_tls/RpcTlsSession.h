// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

// RPC-C2：连接 owner 上的显式 RPC/TLS 组合，握手后接纳、终结前清算已接收帧。
// Intent: rpc_tls.intent.md; ownership/thread affinity/testing rules.
#include "gamenet/rpc/RpcChannel.h"
#include "gamenet/tls/TlsStream.h"

namespace gamenet::rpc {
enum class RpcTlsPhase { Created, Handshaking, Ready, Closing, Settling, Closed };
struct RpcTlsOptions {
    RpcOptions rpc;
    tls::TlsOptions tls;
    std::string peerName; // required DNS/IP identity for a client context
};
struct RpcTlsCallbacks {
    std::function<void()> ready;
    std::function<void(tls::TlsError)> closed; // once, after RPC obligations settle
};
struct RpcTlsSnapshot {
    RpcTlsPhase phase{RpcTlsPhase::Created};
    tls::TlsError terminal{tls::TlsError::None};
    RpcSnapshot rpc;
    tls::TlsSnapshot tls;
    std::uint64_t settlementPosts{}, settlementRejections{}, callbackExceptions{};
};
class RpcTlsSession : public std::enable_shared_from_this<RpcTlsSession> {
public:
    static std::shared_ptr<RpcTlsSession> create(const net::TcpConnectionPtr& connection,
        std::shared_ptr<tls::TlsContext> context, RpcTlsCallbacks callbacks,
        RpcChannel::Handler handler = {}, RpcTlsOptions options = {});
    ~RpcTlsSession(); // owner-only; active abandonment settles Shutdown/Aborted
    RpcTlsSession(const RpcTlsSession&) = delete;
    RpcTlsSession& operator=(const RpcTlsSession&) = delete;
    // Owner-only. Call through channel(); pre-handshake calls return NotReady.
    std::shared_ptr<RpcChannel> channel() const;
    tls::TlsOperation start();
    tls::TlsOperation shutdown();
    tls::TlsOperation abort();
    tls::TlsOperation onMessage(net::Buffer& ciphertext);
    void onWritable();
    void onDisconnect(); // forward actual TCP-down before releasing context
    RpcTlsSnapshot snapshot() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;
private:
    struct Bridge;
    RpcTlsSession(std::shared_ptr<Bridge> bridge, std::shared_ptr<tls::TlsStream> stream);
    std::shared_ptr<Bridge> bridge_;
    std::shared_ptr<tls::TlsStream> stream_;
};
}  // namespace gamenet::rpc
