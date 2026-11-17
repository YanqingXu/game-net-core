// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Implements intents/modules/kcp_endpoint.intent.md and KCP-C2 thread/ownership rules.
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#include <stdexcept>
#include <utility>

namespace gamenet::kcp_transport {
namespace {
kcp::KcpCloseReason reason(transport::CloseReason value) noexcept {
    using R = transport::CloseReason;
    using K = kcp::KcpCloseReason;
    switch (value) {
    case R::Normal: return K::Normal;
    case R::GoingAway: return K::GoingAway;
    case R::ProtocolError: return K::ProtocolError;
    case R::Replaced: return K::GoingAway;
    case R::IdleTimeout: return K::IdleTimeout;
    case R::Overloaded: return K::ResourceExhausted;
    }
    return K::ProtocolError;
}
transport::EndpointResult result(kcp::KcpResult value) noexcept {
    using K = kcp::KcpResult;
    using E = transport::EndpointResult;
    switch (value) {
    case K::Accepted: return E::Accepted;
    case K::Stopped: return E::Closed;
    case K::WrongThread: return E::WrongThread;
    case K::OwnerUnavailable: return E::OwnerUnavailable;
    case K::AlreadyStarted:
    case K::Overloaded:
    case K::InvalidArgument:
    case K::NoMemory: return E::Overloaded;
    }
    return E::Overloaded;
}
} // namespace

KcpTransportEndpoint::KcpTransportEndpoint(std::shared_ptr<kcp::KcpSession> session)
    : session_(std::move(session)) {
    if (!session_) throw std::invalid_argument("KCP endpoint requires session");
}
transport::TransportSessionId KcpTransportEndpoint::id() const noexcept {
    return {session_->identity().peer.id};
}
net::EventLoopExecutor KcpTransportEndpoint::ownerExecutor() const noexcept {
    return session_->ownerExecutor();
}
transport::EndpointResult KcpTransportEndpoint::send(std::string_view bytes) {
    return result(session_->send(bytes).result);
}
transport::EndpointResult KcpTransportEndpoint::close(transport::CloseReason value) {
    return result(session_->close(reason(value)));
}
gamenet::DispatchResult KcpTransportEndpoint::requestClose(transport::CloseReason value) noexcept {
    return session_->requestClose(reason(value));
}
bool KcpTransportEndpoint::isOpen() const noexcept { return session_->isOpen(); }
} // namespace gamenet::kcp_transport
