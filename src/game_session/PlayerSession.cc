#include "gamenet/game_session/PlayerSession.h"

#include <stdexcept>
#include <utility>

namespace gamenet::game_session {

SessionBinding::SessionBinding(
    SessionId sessionId,
    gamenet::transport::TransportSessionId transportId,
    SessionBindingGeneration generation,
    std::shared_ptr<State> state) noexcept
    : sessionId_(sessionId),
      transportId_(transportId),
      generation_(generation),
      state_(std::move(state)) {}

SessionId SessionBinding::sessionId() const noexcept { return sessionId_; }
gamenet::transport::TransportSessionId SessionBinding::transportId() const noexcept {
    return transportId_;
}
SessionBindingGeneration SessionBinding::generation() const noexcept {
    return generation_;
}
bool SessionBinding::tracked() const noexcept {
    return state_ && generation_ != 0;
}
bool SessionBinding::isCurrent() const noexcept {
    return !tracked() ||
        state_->currentGeneration.load(std::memory_order_acquire) == generation_;
}

PlayerSession::PlayerSession(
    SessionId sessionId,
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    SessionBindingGeneration generation,
    Clock::time_point now)
    : sessionId_(sessionId),
      playerId_(std::move(playerId)),
      endpoint_(std::move(endpoint)),
      lastActivity_(now),
      generation_(generation),
      bindingState_(std::make_shared<SessionBinding::State>()) {
    if (playerId_.empty() || !endpoint_ || generation_ == 0) {
        throw std::invalid_argument("PlayerSession requires player id and endpoint");
    }
    bindingState_->currentGeneration.store(generation_, std::memory_order_release);
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
SessionBinding PlayerSession::binding() const noexcept {
    return SessionBinding(sessionId_, transportId(), generation_, bindingState_);
}

void PlayerSession::markOnline(Clock::time_point now) noexcept {
    state_ = SessionState::Online;
    lastActivity_ = now;
}

void PlayerSession::markOffline() noexcept {
    bindingState_->currentGeneration.store(0, std::memory_order_release);
    state_ = SessionState::Offline;
}

void PlayerSession::rebind(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    SessionBindingGeneration generation,
    Clock::time_point now) {
    if (!endpoint || generation == 0) {
        throw std::invalid_argument("PlayerSession rebind requires endpoint");
    }
    bindingState_->currentGeneration.store(0, std::memory_order_release);
    endpoint_ = std::move(endpoint);
    generation_ = generation;
    bindingState_->currentGeneration.store(generation_, std::memory_order_release);
    markOnline(now);
}

void PlayerSession::refreshBinding(
    SessionBindingGeneration generation,
    Clock::time_point now) noexcept {
    bindingState_->currentGeneration.store(0, std::memory_order_release);
    generation_ = generation;
    bindingState_->currentGeneration.store(generation_, std::memory_order_release);
    markOnline(now);
}

void PlayerSession::heartbeat(Clock::time_point now) noexcept { lastActivity_ = now; }

}  // namespace gamenet::game_session
