#include "gamenet/transport/TcpTransportEndpoint.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include <stdexcept>

namespace gamenet::transport {

TcpTransportEndpoint::TcpTransportEndpoint(
    TransportSessionId id,
    const std::shared_ptr<gamenet::net::TcpConnection>& connection)
    : id_(id),
      ownerLoop_(connection ? connection->getLoop() : nullptr),
      connection_(connection) {
    if (!connection || ownerLoop_ == nullptr) {
        throw std::invalid_argument("TcpTransportEndpoint requires a loop-bound connection");
    }
}

TransportSessionId TcpTransportEndpoint::id() const noexcept {
    return id_;
}

gamenet::net::EventLoop* TcpTransportEndpoint::ownerLoop() const noexcept {
    return ownerLoop_;
}

EndpointResult TcpTransportEndpoint::send(std::string_view bytes) {
    if (!ownerLoop_->isInLoopThread()) {
        return EndpointResult::WrongThread;
    }
    auto connection = connection_.lock();
    if (!connection || !connection->connected()) {
        return EndpointResult::Closed;
    }
    connection->send(bytes);
    return EndpointResult::Accepted;
}

EndpointResult TcpTransportEndpoint::close(CloseReason reason) {
    if (!ownerLoop_->isInLoopThread()) {
        return EndpointResult::WrongThread;
    }
    auto connection = connection_.lock();
    if (!connection || connection->disconnected()) {
        return EndpointResult::Closed;
    }
    if (reason == CloseReason::Normal || reason == CloseReason::GoingAway) {
        connection->shutdown();
    } else {
        connection->forceClose();
    }
    return EndpointResult::Accepted;
}

bool TcpTransportEndpoint::isOpen() const noexcept {
    auto connection = connection_.lock();
    return connection && connection->connected();
}

}  // namespace gamenet::transport
