#pragma once

#include <cstdint>
#include <compare>
#include <string_view>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::transport {

struct TransportSessionId {
    std::uint64_t value{};
    auto operator<=>(const TransportSessionId&) const = default;
};

enum class CloseReason {
    Normal,
    GoingAway,
    ProtocolError,
    Replaced,
    IdleTimeout,
    Overloaded,
};

enum class EndpointResult {
    Accepted,
    Closed,
    WrongThread,
};

class TransportEndpoint {
public:
    virtual ~TransportEndpoint() = default;

    virtual TransportSessionId id() const noexcept = 0;
    virtual gamenet::net::EventLoop* ownerLoop() const noexcept = 0;
    virtual EndpointResult send(std::string_view bytes) = 0;
    virtual EndpointResult close(CloseReason reason) = 0;
    virtual bool isOpen() const noexcept = 0;
};

}  // namespace gamenet::transport
