#pragma once

#include "gamenet/transport/TransportEndpoint.h"

#include <memory>

namespace gamenet::net {
class TcpConnection;
}

namespace gamenet::transport {

class TcpTransportEndpoint final : public TransportEndpoint {
public:
    TcpTransportEndpoint(
        TransportSessionId id,
        const std::shared_ptr<gamenet::net::TcpConnection>& connection);

    TransportSessionId id() const noexcept override;
    gamenet::net::EventLoop* ownerLoop() const noexcept override;
    EndpointResult send(std::string_view bytes) override;
    EndpointResult close(CloseReason reason) override;
    bool isOpen() const noexcept override;

private:
    TransportSessionId id_;
    gamenet::net::EventLoop* ownerLoop_;
    std::weak_ptr<gamenet::net::TcpConnection> connection_;
};

}  // namespace gamenet::transport
