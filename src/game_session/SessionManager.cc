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
      options_(options),
      idleDeadlines_(ownerLoop
              ? std::make_unique<gamenet::net::DeadlineQueue>(
                    ownerLoop,
                    gamenet::net::DeadlineQueueOptions{
                        .resolution = options.idleDeadlineResolution,
                        .maxExpiredPerAdvance =
                            options.maxIdleExpirationsPerAdvance,
                    })
              : nullptr) {
    if (!ownerLoop_) {
        throw std::invalid_argument("SessionManager requires an owner loop");
    }
    if (options_.idleTimeout <= Clock::duration::zero()) {
        throw std::invalid_argument(
            "SessionManager idle timeout must be positive");
    }
}

SessionManager::~SessionManager() {
    if (!ownerLoop_->isInLoopThread()) {
        LOG_FATAL << "SessionManager destroyed outside its management EventLoop";
    }
    lifetimeState_->revoke(gamenet::DispatchResult::OwnerUnavailable);
}

gamenet::net::EventLoop* SessionManager::ownerLoop() const noexcept { return ownerLoop_; }

AuthenticateResult SessionManager::authenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    Clock::time_point now) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return {.dispatch = gamenet::DispatchResult::Shutdown};
    }
    if (playerId.empty() || !endpoint) {
        return {.dispatch = gamenet::DispatchResult::PolicyRejected};
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
            return {
                .status = AuthenticateStatus::Rejected,
                .session = nullptr,
                .dispatch = gamenet::DispatchResult::PolicyRejected};
        }
    }

    const auto existing = byPlayer_.find(playerId);
    if (existing == byPlayer_.end()) {
        auto session = std::shared_ptr<PlayerSession>(new PlayerSession(
            nextSessionId_++,
            std::move(playerId),
            std::move(endpoint),
            nextBindingGeneration_++,
            now));
        session->markOnline(now);
        session->idleDeadline_ = idleDeadlines_->schedule(
            session->transportId().value,
            now + options_.idleTimeout);
        byTransport_[session->transportId().value] = session;
        byPlayer_[session->playerId()] = session;
        return {
            .status = AuthenticateStatus::Created,
            .session = std::move(session),
            .dispatch = gamenet::DispatchResult::Accepted};
    }

    auto session = existing->second;
    if (session->transportId() == endpoint->id()) {
        session->refreshBinding(nextBindingGeneration_++, now);
        session->idleDeadline_ = idleDeadlines_->schedule(
            session->transportId().value,
            now + options_.idleTimeout);
        return {
            .status = AuthenticateStatus::Existing,
            .session = std::move(session),
            .dispatch = gamenet::DispatchResult::Accepted};
    }
    if (options_.duplicateLogin == DuplicateLoginPolicy::RejectNew) {
        return {
            .status = AuthenticateStatus::Rejected,
            .session = std::move(session),
            .dispatch = gamenet::DispatchResult::PolicyRejected};
    }

    auto previousEndpoint = session->endpoint();
    (void)idleDeadlines_->cancel(session->idleDeadline_);
    byTransport_.erase(session->transportId().value);
    session->rebind(std::move(endpoint), nextBindingGeneration_++, now);
    session->idleDeadline_ = idleDeadlines_->schedule(
        session->transportId().value,
        now + options_.idleTimeout);
    byTransport_[session->transportId().value] = session;
    closeEndpoint(std::move(previousEndpoint), gamenet::transport::CloseReason::Replaced);
    return {
        .status = AuthenticateStatus::Rebound,
        .session = std::move(session),
        .dispatch = gamenet::DispatchResult::Accepted};
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
    session->idleDeadline_ = idleDeadlines_->schedule(
        session->transportId().value,
        now + options_.idleTimeout);
    return true;
}

std::size_t SessionManager::expireIdle(Clock::time_point now) {
    std::size_t expired = 0;
    bool readyRemaining = false;
    do {
        const auto batch = expireIdleBatch(now);
        expired += batch.expired;
        readyRemaining = batch.readyRemaining;
    } while (readyRemaining);
    return expired;
}

IdleExpirationResult SessionManager::expireIdleBatch(
    Clock::time_point now) {
    assertOwner();
    if (lifecycleState_ != LifecycleState::Running) {
        return {};
    }

    auto ready = idleDeadlines_->advance(now);
    std::size_t expired = 0;
    for (const auto& expiration : ready.expired) {
        auto session = findMutableByTransport(
            {expiration.token.key});
        if (!session ||
            session->idleDeadline_ != expiration.token) {
            continue;
        }
        if (now - session->lastActivity() < options_.idleTimeout) {
            session->idleDeadline_ = idleDeadlines_->schedule(
                session->transportId().value,
                session->lastActivity() + options_.idleTimeout);
            continue;
        }

        auto endpoint = session->endpoint();
        eraseSession(session);
        closeEndpoint(std::move(endpoint), gamenet::transport::CloseReason::IdleTimeout);
        ++expired;
    }
    return {
        .expired = expired,
        .readyRemaining = ready.readyRemaining,
    };
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
    lifetimeState_->revoke(gamenet::DispatchResult::Shutdown);
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
    idleDeadlines_->clear();

    for (auto& endpoint : endpoints) {
        closeEndpoint(std::move(endpoint), gamenet::transport::CloseReason::GoingAway);
    }
}

gamenet::DispatchResult SessionManager::postAuthenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    AuthenticateCallback callback) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state) return gamenet::DispatchResult::OwnerUnavailable;
    if (!state->active()) return state->result();
    const auto posted = ownerExecutor_.post([
        this,
        lifetime,
        playerId = std::move(playerId),
        endpoint = std::move(endpoint),
        callback = std::move(callback)]() mutable {
        const auto state = lifetime.lock();
        if (!state) {
            if (callback) {
                callback({.dispatch = gamenet::DispatchResult::OwnerUnavailable});
            }
            return;
        }
        if (!state->active()) {
            if (callback) {
                callback({.dispatch = state->result()});
            }
            return;
        }
        auto result = authenticate(std::move(playerId), std::move(endpoint));
        if (callback) {
            callback(std::move(result));
        }
    });
    return gamenet::dispatchResult(posted);
}

gamenet::DispatchResult SessionManager::postOffline(
    gamenet::transport::TransportSessionId transportId,
    MutationCallback callback) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state) return gamenet::DispatchResult::OwnerUnavailable;
    if (!state->active()) return state->result();
    const auto posted = ownerExecutor_.post(
        [this, lifetime, transportId, callback = std::move(callback)]() mutable {
        const auto state = lifetime.lock();
        if (!state) {
            if (callback) callback(gamenet::DispatchResult::OwnerUnavailable);
            return;
        }
        if (!state->active()) {
            if (callback) callback(state->result());
            return;
        }
        const auto result = offline(transportId)
            ? gamenet::DispatchResult::Accepted
            : gamenet::DispatchResult::PolicyRejected;
        if (callback) callback(result);
    });
    return gamenet::dispatchResult(posted);
}

gamenet::DispatchResult SessionManager::postHeartbeat(
    gamenet::transport::TransportSessionId transportId,
    MutationCallback callback) {
    const std::weak_ptr<LifetimeState> lifetime = lifetimeState_;
    const auto state = lifetime.lock();
    if (!state) return gamenet::DispatchResult::OwnerUnavailable;
    if (!state->active()) return state->result();
    const auto posted = ownerExecutor_.post(
        [this, lifetime, transportId, callback = std::move(callback)]() mutable {
        const auto state = lifetime.lock();
        if (!state) {
            if (callback) callback(gamenet::DispatchResult::OwnerUnavailable);
            return;
        }
        if (!state->active()) {
            if (callback) callback(state->result());
            return;
        }
        const auto result = heartbeat(transportId)
            ? gamenet::DispatchResult::Accepted
            : gamenet::DispatchResult::PolicyRejected;
        if (callback) callback(result);
    });
    return gamenet::dispatchResult(posted);
}

void SessionManager::assertOwner() const { ownerLoop_->assertInLoopThread(); }

void SessionManager::closeEndpoint(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    gamenet::transport::CloseReason reason) {
    if (!endpoint) {
        return;
    }
    (void)endpoint->requestClose(reason);
}

void SessionManager::eraseSession(const std::shared_ptr<PlayerSession>& session) {
    (void)idleDeadlines_->cancel(session->idleDeadline_);
    session->idleDeadline_ = {};
    byTransport_.erase(session->transportId().value);
    const auto player = byPlayer_.find(session->playerId());
    if (player != byPlayer_.end() && player->second == session) {
        byPlayer_.erase(player);
    }
    session->markOffline();
}

}  // namespace gamenet::game_session
