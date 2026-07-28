#pragma once

#include "gamenet/transport/TransportEndpoint.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace gamenet::game_session {

using PlayerId = std::string;
using SessionId = std::uint64_t;
using SessionBindingGeneration = std::uint64_t;

enum class SessionState {
    Authenticating,
    Online,
    Offline,
};

class PlayerSession;
class SessionManager;

// Immutable cross-thread validation capability for one exact session binding.
// It owns no session, endpoint, manager, or EventLoop.
class SessionBinding {
public:
    SessionBinding() = default;

    SessionId sessionId() const noexcept;
    gamenet::transport::TransportSessionId transportId() const noexcept;
    SessionBindingGeneration generation() const noexcept;
    bool tracked() const noexcept;
    bool isCurrent() const noexcept;

private:
    struct State {
        std::atomic<SessionBindingGeneration> currentGeneration{0};
    };

    SessionBinding(
        SessionId sessionId,
        gamenet::transport::TransportSessionId transportId,
        SessionBindingGeneration generation,
        std::shared_ptr<State> state) noexcept;

    SessionId sessionId_{};
    gamenet::transport::TransportSessionId transportId_{};
    SessionBindingGeneration generation_{};
    std::shared_ptr<State> state_;

    friend class PlayerSession;
    friend class SessionManager;
};

// SessionManager owns every PlayerSession on its management EventLoop.
// All PlayerSession accessors and mutations are management-loop-only; const access and a
// shared_ptr<const PlayerSession> prevent unauthorized mutation but do not form
// a cross-thread snapshot. Copy value data before handing it to another loop.
class PlayerSession {
public:
    using Clock = std::chrono::steady_clock;

    SessionId sessionId() const noexcept;
    const PlayerId& playerId() const noexcept;
    SessionState state() const noexcept;
    gamenet::transport::TransportSessionId transportId() const noexcept;
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint() const noexcept;
    Clock::time_point lastActivity() const noexcept;
    SessionBinding binding() const noexcept;

private:
    PlayerSession(
        SessionId sessionId,
        PlayerId playerId,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        SessionBindingGeneration generation,
        Clock::time_point now);
    void markOnline(Clock::time_point now) noexcept;
    void markOffline() noexcept;
    void rebind(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        SessionBindingGeneration generation,
        Clock::time_point now);
    void refreshBinding(SessionBindingGeneration generation, Clock::time_point now) noexcept;
    void heartbeat(Clock::time_point now) noexcept;

    SessionId sessionId_;
    PlayerId playerId_;
    SessionState state_{SessionState::Authenticating};
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint_;
    Clock::time_point lastActivity_;
    SessionBindingGeneration generation_{};
    std::shared_ptr<SessionBinding::State> bindingState_;

    friend class SessionManager;
};

}  // namespace gamenet::game_session
