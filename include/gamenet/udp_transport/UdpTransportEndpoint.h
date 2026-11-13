// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// UDP 与 TransportEndpoint 的可选组合边界；不改变 UDP 本身的 Core-only 依赖。
#include "gamenet/transport/TransportEndpoint.h"
#include "gamenet/udp/UdpPeer.h"
namespace gamenet::udp_transport {
class UdpTransportEndpoint final : public transport::TransportEndpoint {
  public:
    explicit UdpTransportEndpoint(std::shared_ptr<udp::UdpPeer>);
    transport::TransportSessionId id() const noexcept override;
    net::EventLoopExecutor ownerExecutor() const noexcept override;
    transport::EndpointResult send(std::string_view) override;
    transport::EndpointResult close(transport::CloseReason) override;
    gamenet::DispatchResult requestClose(transport::CloseReason) noexcept override;
    bool isOpen() const noexcept override;

  private:
    std::shared_ptr<udp::UdpPeer> peer_;
};
} // namespace gamenet::udp_transport
