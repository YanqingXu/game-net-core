#pragma once

#include "gamenet/core/net/EventLoopExecutor.h"

#include <cstdint>
#include <compare>
#include <string_view>

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
    // The observed connection object/lifecycle is already closed. For the TCP
    // adapter, an expired weak connection takes precedence over owner state.
    Closed,
    // A live owner is available, but the caller is not its thread.
    WrongThread,
    // The connection object may still exist, but owner admission and its final
    // accepted-work drain have ended, so no owner operation is permitted.
    OwnerUnavailable,
};

class TransportEndpoint {
public:
    virtual ~TransportEndpoint() = default;

    virtual TransportSessionId id() const noexcept = 0;
    virtual gamenet::net::EventLoopExecutor ownerExecutor() const noexcept = 0;
    virtual EndpointResult send(std::string_view bytes) = 0;
    virtual EndpointResult close(CloseReason reason) = 0;
    // Cross-thread-safe snapshot observer. Implementations must not expose
    // unsynchronized owner-loop state through this query.
    virtual bool isOpen() const noexcept = 0;
};

}  // namespace gamenet::transport
