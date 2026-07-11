#include "gamenet/transport/TcpTransportEndpoint.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include <stdexcept>

namespace gamenet::transport {

TcpTransportEndpoint::TcpTransportEndpoint(
    TransportSessionId id,
    const std::shared_ptr<gamenet::net::TcpConnection>& connection)
    : id_(id), connection_(connection) {
    if (!connection || !connection->getLoop()) {
        throw std::invalid_argument("TcpTransportEndpoint requires a loop-bound connection");
    }
    auto* ownerLoop = connection->getLoop();
    ownerLoop->assertInLoopThread();
    ownerExecutor_ = ownerLoop->executor();
    if (!ownerExecutor_.available()) {
        throw std::invalid_argument("TcpTransportEndpoint requires an accepting owner loop");
    }
}

TransportSessionId TcpTransportEndpoint::id() const noexcept {
    return id_;
}

gamenet::net::EventLoopExecutor TcpTransportEndpoint::ownerExecutor() const noexcept {
    return ownerExecutor_;
}

EndpointResult TcpTransportEndpoint::send(std::string_view bytes) {
    auto connection = connection_.lock();
    if (!connection) return EndpointResult::Closed;
    if (!ownerExecutor_.isInOwnerThread()) {
        return ownerExecutor_.available() ? EndpointResult::WrongThread
                                          : EndpointResult::OwnerUnavailable;
    }
    if (!connection->connected()) {
        return EndpointResult::Closed;
    }
    connection->send(bytes);
    return EndpointResult::Accepted;
}

EndpointResult TcpTransportEndpoint::close(CloseReason reason) {
    auto connection = connection_.lock();
    if (!connection) return EndpointResult::Closed;
    if (!ownerExecutor_.isInOwnerThread()) {
        return ownerExecutor_.available() ? EndpointResult::WrongThread
                                          : EndpointResult::OwnerUnavailable;
    }
    if (connection->disconnected()) {
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
    return connection && ownerExecutor_.available() && connection->connected();
}

}  // namespace gamenet::transport
