#pragma once

#include "gamenet/transport/TransportEndpoint.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace gamenet::game_session {

using PlayerId = std::string;
using SessionId = std::uint64_t;

enum class SessionState {
    Authenticating,
    Online,
    Offline,
};

// SessionManager owns every PlayerSession on its management EventLoop.
// All PlayerSession accessors and mutations are management-loop-only; const access and a
// shared_ptr<const PlayerSession> prevent unauthorized mutation but do not form
// a cross-thread snapshot. Copy value data before handing it to another loop.
class PlayerSession {
public:
    using Clock = std::chrono::steady_clock;

    PlayerSession(
        SessionId sessionId,
        PlayerId playerId,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        Clock::time_point now);

    SessionId sessionId() const noexcept;
    const PlayerId& playerId() const noexcept;
    SessionState state() const noexcept;
    gamenet::transport::TransportSessionId transportId() const noexcept;
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint() const noexcept;
    Clock::time_point lastActivity() const noexcept;

    void markOnline(Clock::time_point now) noexcept;
    void markOffline() noexcept;
    void rebind(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        Clock::time_point now);
    void heartbeat(Clock::time_point now) noexcept;

private:
    SessionId sessionId_;
    PlayerId playerId_;
    SessionState state_{SessionState::Authenticating};
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint_;
    Clock::time_point lastActivity_;
};

}  // namespace gamenet::game_session
