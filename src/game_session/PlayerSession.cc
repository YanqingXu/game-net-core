#include "gamenet/game_session/PlayerSession.h"

#include <stdexcept>
#include <utility>

namespace gamenet::game_session {

PlayerSession::PlayerSession(
    SessionId sessionId,
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    Clock::time_point now)
    : sessionId_(sessionId),
      playerId_(std::move(playerId)),
      endpoint_(std::move(endpoint)),
      lastActivity_(now) {
    if (playerId_.empty() || !endpoint_) {
        throw std::invalid_argument("PlayerSession requires player id and endpoint");
    }
}

SessionId PlayerSession::sessionId() const noexcept { return sessionId_; }
const PlayerId& PlayerSession::playerId() const noexcept { return playerId_; }
SessionState PlayerSession::state() const noexcept { return state_; }
gamenet::transport::TransportSessionId PlayerSession::transportId() const noexcept {
    return endpoint_->id();
}
const std::shared_ptr<gamenet::transport::TransportEndpoint>& PlayerSession::endpoint() const noexcept {
    return endpoint_;
}
PlayerSession::Clock::time_point PlayerSession::lastActivity() const noexcept { return lastActivity_; }

void PlayerSession::markOnline(Clock::time_point now) noexcept {
    state_ = SessionState::Online;
    lastActivity_ = now;
}

void PlayerSession::markOffline() noexcept { state_ = SessionState::Offline; }

void PlayerSession::rebind(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    Clock::time_point now) {
    if (!endpoint) {
        throw std::invalid_argument("PlayerSession rebind requires endpoint");
    }
    endpoint_ = std::move(endpoint);
    markOnline(now);
}

void PlayerSession::heartbeat(Clock::time_point now) noexcept { lastActivity_ = now; }

}  // namespace gamenet::game_session
