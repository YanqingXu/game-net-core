// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Implements intents/modules/udp_transport.intent.md; owns only a weak-server peer handle.
#include "gamenet/udp_transport/UdpTransportEndpoint.h"
#include <stdexcept>
namespace gamenet::udp_transport {
namespace {
udp::UdpPeerCloseReason reason(transport::CloseReason r) noexcept {
    using R = transport::CloseReason;
    using U = udp::UdpPeerCloseReason;
    switch (r) {
    case R::Normal:
        return U::Normal;
    case R::GoingAway:
        return U::GoingAway;
    case R::ProtocolError:
        return U::ProtocolError;
    case R::Replaced:
        return U::Replaced;
    case R::IdleTimeout:
        return U::IdleTimeout;
    case R::Overloaded:
        return U::Overloaded;
    }
    return U::ProtocolError;
}
transport::EndpointResult result(udp::UdpResult r) noexcept {
    using U = udp::UdpResult;
    using E = transport::EndpointResult;
    switch (r) {
    case U::Accepted:
        return E::Accepted;
    case U::Stopped:
        return E::Closed;
    case U::WrongThread:
        return E::WrongThread;
    case U::OwnerUnavailable:
        return E::OwnerUnavailable;
    default:
        return E::Overloaded;
    }
}
} // namespace
UdpTransportEndpoint::UdpTransportEndpoint(std::shared_ptr<udp::UdpPeer> p) : peer_(std::move(p)) {
    if (!peer_)
        throw std::invalid_argument("UDP endpoint requires peer");
}
transport::TransportSessionId UdpTransportEndpoint::id() const noexcept { return {peer_->id()}; }
net::EventLoopExecutor UdpTransportEndpoint::ownerExecutor() const noexcept {
    return peer_->ownerExecutor();
}
transport::EndpointResult UdpTransportEndpoint::send(std::string_view b) {
    return result(peer_->send(b));
}
transport::EndpointResult UdpTransportEndpoint::close(transport::CloseReason r) {
    return result(peer_->close(reason(r)));
}
gamenet::DispatchResult UdpTransportEndpoint::requestClose(transport::CloseReason r) noexcept {
    return peer_->requestClose(reason(r));
}
bool UdpTransportEndpoint::isOpen() const noexcept { return peer_->isOpen(); }
} // namespace gamenet::udp_transport
