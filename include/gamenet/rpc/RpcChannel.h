// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Implements intents/modules/rpc.intent.md (RPC-C1), ownership/thread rules.
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/rpc/RpcCodec.h"

#include <chrono>
#include <functional>
#include <memory>

namespace gamenet::net { class Buffer; }
namespace gamenet::rpc {
class RpcTransport;

enum class RpcStatus {
    Ok, RemoteError, Timeout, Cancelled, Disconnected, Shutdown,
    ProtocolError, SchedulingRejected,
};
enum class RpcOperation { Accepted, Closed, WrongThread, InvalidArgument,
    PendingLimit, SendRejected, OwnerUnavailable, IdExhausted, NotFound, NotReady };
struct RpcResult { RpcStatus status{RpcStatus::Ok}; std::string payload; };
struct RpcCallResult {
    RpcOperation status{RpcOperation::Closed};
    std::uint64_t requestId{};  // zero for rejected admission (no callback)
};
struct RpcOptions {
    std::size_t maxPayloadBytes{RpcCodec::kDefaultMaxPayloadBytes};
    std::size_t maxPendingRequests{256};
    std::size_t maxPendingBytes{1024 * 1024};
    std::size_t maxInboundRequests{256};
    std::size_t maxInboundBytes{1024 * 1024};
    std::size_t maxFramesPerDispatch{32};
    std::size_t maxBufferedBytes{256 * 1024};
    std::chrono::milliseconds maxRequestTimeout{60000};
};
struct RpcSnapshot {
    bool closed{};
    std::size_t pendingRequests{}, pendingBytes{}, inboundRequests{}, inboundBytes{};
    std::uint64_t acceptedCalls{}, completedCalls{}, ignoredResponses{};
    std::uint64_t callbackExceptions{}, handlerExceptions{}, inboundRejected{};
    std::uint64_t continuationRejections{}, reentrantMessages{};
    std::uint64_t transportExceptions{};
    bool dispatching{}, continuationQueued{};
};

class RpcChannel : public std::enable_shared_from_this<RpcChannel> {
public:
    using Completion = std::function<void(RpcResult)>;
    // Only Ok/RemoteError are valid reply statuses; owner-only, copied handles
    // are weak and return NotFound after first reply/expiry, Closed after close.
    using Reply = std::function<RpcOperation(RpcResult)>;
    using Handler = std::function<void(const RpcMessage&, Reply)>;
    static std::shared_ptr<RpcChannel> create(
        const net::TcpConnectionPtr& connection, Handler handler = {}, RpcOptions options = {});
    static std::shared_ptr<RpcChannel> createWithTransport(
        std::shared_ptr<RpcTransport> transport, Handler handler = {}, RpcOptions options = {});
    ~RpcChannel();  // owner-only; settles remaining callbacks as Shutdown
    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;

    // All operations except ownerExecutor are owner-only. Cross-thread callers
    // post owned values through ownerExecutor and check bounded admission.
    RpcCallResult call(std::uint32_t method, std::string_view payload,
        std::chrono::milliseconds timeout, Completion completion);
    RpcOperation cancel(std::uint64_t requestId);
    RpcOperation close();  // seals RPC; application still owns TCP shutdown
    void onMessage(net::Buffer& input);  // wire into the TCP message callback
    void onDisconnect();  // wire into the TCP down callback; idempotent
    RpcSnapshot snapshot() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;

private:
    struct Impl;
    explicit RpcChannel(std::unique_ptr<Impl> impl);
    void drain();
    void expire(std::uint64_t id, bool inbound);
    RpcOperation reply(std::uint64_t id, RpcResult result);
    std::unique_ptr<Impl> impl_;
};

}  // namespace gamenet::rpc
