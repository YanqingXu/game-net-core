// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// KCP 与 TransportEndpoint 的可选组合；仅持有弱观察 transport 的会话句柄。
#include "gamenet/kcp/KcpTransport.h"
#include "gamenet/transport/TransportEndpoint.h"

namespace gamenet::kcp_transport {
// Implements intents/modules/kcp_endpoint.intent.md; installs no callbacks or queues.
class KcpTransportEndpoint final : public transport::TransportEndpoint {
  public:
    explicit KcpTransportEndpoint(std::shared_ptr<kcp::KcpSession>);
    KcpTransportEndpoint(const KcpTransportEndpoint&) = delete;
    KcpTransportEndpoint& operator=(const KcpTransportEndpoint&) = delete;
    KcpTransportEndpoint(KcpTransportEndpoint&&) = delete;
    KcpTransportEndpoint& operator=(KcpTransportEndpoint&&) = delete;
    transport::TransportSessionId id() const noexcept override;
    net::EventLoopExecutor ownerExecutor() const noexcept override;
    // Owner-only; Accepted is copied message admission, not remote application delivery.
    transport::EndpointResult send(std::string_view) override;
    transport::EndpointResult close(transport::CloseReason) override;
    // Thread-safe lifecycle seal; first reason wins even under normal-queue saturation.
    gamenet::DispatchResult requestClose(transport::CloseReason) noexcept override;
    bool isOpen() const noexcept override;

  private:
    std::shared_ptr<kcp::KcpSession> session_;
};
} // namespace gamenet::kcp_transport
