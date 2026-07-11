#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace gamenet::game_session {

SessionManager::SessionManager(gamenet::net::EventLoop* ownerLoop)
    : SessionManager(ownerLoop, Options{}) {}

SessionManager::SessionManager(gamenet::net::EventLoop* ownerLoop, Options options)
    : ownerLoop_(ownerLoop), options_(options) {
    if (!ownerLoop_) {
        throw std::invalid_argument("SessionManager requires an owner loop");
    }
}

gamenet::net::EventLoop* SessionManager::ownerLoop() const noexcept { return ownerLoop_; }

AuthenticateResult SessionManager::authenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    Clock::time_point now) {
    assertOwner();
    if (playerId.empty() || !endpoint) {
        return {};
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
    auto session = findByTransport(transportId);
    if (!session || session->transportId() != transportId) {
        return false;
    }
    session->heartbeat(now);
    return true;
}

std::size_t SessionManager::expireIdle(Clock::time_point now) {
    assertOwner();
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

std::shared_ptr<PlayerSession> SessionManager::findByPlayer(const PlayerId& playerId) const {
    assertOwner();
    const auto found = byPlayer_.find(playerId);
    return found == byPlayer_.end() ? nullptr : found->second;
}

std::shared_ptr<PlayerSession> SessionManager::findByTransport(
    gamenet::transport::TransportSessionId transportId) const {
    assertOwner();
    const auto found = byTransport_.find(transportId.value);
    return found == byTransport_.end() ? nullptr : found->second;
}

std::size_t SessionManager::size() const {
    assertOwner();
    return byPlayer_.size();
}

void SessionManager::postAuthenticate(
    PlayerId playerId,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    AuthenticateCallback callback) {
    ownerLoop_->queueInLoop([
        this,
        playerId = std::move(playerId),
        endpoint = std::move(endpoint),
        callback = std::move(callback)]() mutable {
        auto result = authenticate(std::move(playerId), std::move(endpoint));
        if (callback) {
            callback(std::move(result));
        }
    });
}

void SessionManager::postOffline(gamenet::transport::TransportSessionId transportId) {
    ownerLoop_->queueInLoop([this, transportId] { offline(transportId); });
}

void SessionManager::postHeartbeat(gamenet::transport::TransportSessionId transportId) {
    ownerLoop_->queueInLoop([this, transportId] { heartbeat(transportId); });
}

void SessionManager::assertOwner() const { ownerLoop_->assertInLoopThread(); }

void SessionManager::closeEndpoint(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    gamenet::transport::CloseReason reason) {
    if (!endpoint || !endpoint->ownerLoop()) {
        return;
    }
    endpoint->ownerLoop()->queueInLoop(
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
