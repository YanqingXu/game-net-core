#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/base/Logger.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::game_session {

SessionManager::SessionManager(gamenet::net::EventLoop* ownerLoop)
    : SessionManager(ownerLoop, Options{}) {}

SessionManager::SessionManager(gamenet::net::EventLoop* ownerLoop, Options options)
    : ownerLoop_(ownerLoop),
      ownerExecutor_(ownerLoop ? ownerLoop->executor() : gamenet::net::EventLoopExecutor{}),
      lifetimeState_(std::make_shared<LifetimeState>()),
      options_(options) {
    if (!ownerLoop_) {
        throw std::invalid_argument("SessionManager requires an owner loop");
    }
}

SessionManager::~SessionManager() {
    if (!ownerLoop_->isInLoopThread()) {
        LOG_FATAL << "SessionManager destroyed outside its management EventLoop";
    }
    lifetimeState_->revoke();
}

gamenet::net::EventLoop* SessionManager::ownerLoop() const noexcept { return ownerLoop_; }

AuthenticateResult SessionManager::authenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    Clock::time_point now) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return {};
    }
    if (playerId.empty() || !endpoint) {
        return {};
    }

    const auto transportOwner = byTransport_.find(endpoint->id().value);
    if (transportOwner != byTransport_.end()) {
        const auto& boundSession = transportOwner->second;
        const bool sameBinding =
            boundSession->playerId() == playerId && boundSession->endpoint() == endpoint;
        if (!sameBinding) {
            if (boundSession->endpoint() != endpoint) {
                closeEndpoint(endpoint, gamenet::transport::CloseReason::ProtocolError);
            }
            return {.status = AuthenticateStatus::Rejected, .session = nullptr};
        }
    }

    const auto existing = byPlayer_.find(playerId);
    if (existing == byPlayer_.end()) {
        auto session = std::make_shared<PlayerSession>(
            nextSessionId_++, std::move(playerId), std::move(endpoint), now);
        session->markOnline(now);
        byTransport_[session->transportId().value] = session;
        byPlayer_[session->playerId()] = session;
        return {.status = AuthenticateStatus::Created, .session = std::move(session)};
    }

    auto session = existing->second;
    if (session->transportId() == endpoint->id()) {
        session->heartbeat(now);
        return {.status = AuthenticateStatus::Existing, .session = std::move(session)};
    }
    if (options_.duplicateLogin == DuplicateLoginPolicy::RejectNew) {
        return {.status = AuthenticateStatus::Rejected, .session = std::move(session)};
    }

    auto previousEndpoint = session->endpoint();
    byTransport_.erase(session->transportId().value);
    session->rebind(std::move(endpoint), now);
    byTransport_[session->transportId().value] = session;
    closeEndpoint(std::move(previousEndpoint), gamenet::transport::CloseReason::Replaced);
    return {.status = AuthenticateStatus::Rebound, .session = std::move(session)};
}

bool SessionManager::offline(gamenet::transport::TransportSessionId transportId) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return false;
    }
    const auto found = byTransport_.find(transportId.value);
    if (found == byTransport_.end()) {
        return false;
    }
    auto session = found->second;
    if (session->transportId() != transportId) {
        return false;
    }
    eraseSession(session);
    return true;
}

bool SessionManager::heartbeat(
    gamenet::transport::TransportSessionId transportId,
    Clock::time_point now) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return false;
    }
    auto session = findMutableByTransport(transportId);
    if (!session || session->transportId() != transportId) {
        return false;
    }
    session->heartbeat(now);
    return true;
}

std::size_t SessionManager::expireIdle(Clock::time_point now) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return 0;
    }
    std::vector<std::shared_ptr<PlayerSession>> expired;
    for (const auto& [playerId, session] : byPlayer_) {
        (void)playerId;
        if (now - session->lastActivity() >= options_.idleTimeout) {
            expired.push_back(session);
        }
    }
    for (const auto& session : expired) {
        auto endpoint = session->endpoint();
        eraseSession(session);
        closeEndpoint(std::move(endpoint), gamenet::transport::CloseReason::IdleTimeout);
    }
    return expired.size();
}

std::shared_ptr<const PlayerSession> SessionManager::findByPlayer(const PlayerId& playerId) const {
    assertOwner();
    const auto found = byPlayer_.find(playerId);
    return found == byPlayer_.end() ? nullptr : found->second;
}

std::shared_ptr<const PlayerSession> SessionManager::findByTransport(
    gamenet::transport::TransportSessionId transportId) const {
    return findMutableByTransport(transportId);
}

std::shared_ptr<PlayerSession> SessionManager::findMutableByTransport(
    gamenet::transport::TransportSessionId transportId) const {
    assertOwner();
    const auto found = byTransport_.find(transportId.value);
    return found == byTransport_.end() ? nullptr : found->second;
}

std::size_t SessionManager::size() const {
    assertOwner();
    return byPlayer_.size();
}

void SessionManager::shutdown() {
    assertOwner();
    if (lifecycleState_ == LifecycleState::Shutdown) {
        return;
    }
    lifetimeState_->revoke();
    lifecycleState_ = LifecycleState::Shutdown;

    std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> endpoints;
    endpoints.reserve(byPlayer_.size());
    for (const auto& [playerId, session] : byPlayer_) {
        (void)playerId;
        endpoints.push_back(session->endpoint());
        session->markOffline();
    }
    byTransport_.clear();
    byPlayer_.clear();

    for (auto& endpoint : endpoints) {
        closeEndpoint(std::move(endpoint), gamenet::transport::CloseReason::GoingAway);
    }
}

void SessionManager::postAuthenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    AuthenticateCallback callback) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state || !state->active()) return;
    (void)ownerExecutor_.tryQueue([
        this,
        lifetime,
        playerId = std::move(playerId),
        endpoint = std::move(endpoint),
        callback = std::move(callback)]() mutable {
        const auto state = lifetime.lock();
        if (!state || !state->active()) return;
        auto result = authenticate(std::move(playerId), std::move(endpoint));
        if (callback) {
            callback(std::move(result));
        }
    });
}

void SessionManager::postOffline(gamenet::transport::TransportSessionId transportId) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state || !state->active()) return;
    (void)ownerExecutor_.tryQueue([this, lifetime, transportId] {
        const auto state = lifetime.lock();
        if (!state || !state->active()) return;
        offline(transportId);
    });
}

void SessionManager::postHeartbeat(gamenet::transport::TransportSessionId transportId) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state || !state->active()) return;
    (void)ownerExecutor_.tryQueue([this, lifetime, transportId] {
        const auto state = lifetime.lock();
        if (!state || !state->active()) return;
        heartbeat(transportId);
    });
}

void SessionManager::assertOwner() const { ownerLoop_->assertInLoopThread(); }

void SessionManager::closeEndpoint(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    gamenet::transport::CloseReason reason) {
    if (!endpoint) {
        return;
    }
    const auto executor = endpoint->ownerExecutor();
    (void)executor.tryQueue(
        [endpoint = std::move(endpoint), reason] { endpoint->close(reason); });
}

void SessionManager::eraseSession(const std::shared_ptr<PlayerSession>& session) {
    byTransport_.erase(session->transportId().value);
    const auto player = byPlayer_.find(session->playerId());
    if (player != byPlayer_.end() && player->second == session) {
        byPlayer_.erase(player);
    }
    session->markOffline();
}

}  // namespace gamenet::game_session
