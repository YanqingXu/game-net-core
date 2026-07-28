#pragma once

#include "gamenet/core/DispatchResult.h"
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
    // The endpoint is live, but its bounded output admission is saturated.
    Overloaded,
};

class TransportEndpoint {
public:
    virtual ~TransportEndpoint() = default;

    virtual TransportSessionId id() const noexcept = 0;
    virtual gamenet::net::EventLoopExecutor ownerExecutor() const noexcept = 0;
    virtual EndpointResult send(std::string_view bytes) = 0;
    virtual EndpointResult close(CloseReason reason) = 0;
    // Terminal control path. Concrete transports with a control/lifecycle lane
    // override this for cross-thread progress. The generic fallback is inline
    // only and rejects a cross-thread request rather than hiding normal-queue
    // scheduling.
    virtual gamenet::DispatchResult requestClose(CloseReason reason) noexcept {
        const auto executor = ownerExecutor();
        if (!executor.isInOwnerThread()) {
            return executor.available()
                ? gamenet::DispatchResult::PolicyRejected
                : gamenet::DispatchResult::OwnerUnavailable;
        }
        switch (close(reason)) {
        case EndpointResult::Accepted:
            return gamenet::DispatchResult::Accepted;
        case EndpointResult::Closed:
            return gamenet::DispatchResult::EndpointClosed;
        case EndpointResult::OwnerUnavailable:
            return gamenet::DispatchResult::OwnerUnavailable;
        case EndpointResult::Overloaded:
            return gamenet::DispatchResult::EndpointOverloaded;
        case EndpointResult::WrongThread:
            return gamenet::DispatchResult::PolicyRejected;
        }
        return gamenet::DispatchResult::PolicyRejected;
    }
    // Cross-thread-safe snapshot observer. Implementations must not expose
    // unsynchronized owner-loop state through this query.
    virtual bool isOpen() const noexcept = 0;
};

}  // namespace gamenet::transport
