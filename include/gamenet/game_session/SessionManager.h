#pragma once

#include "gamenet/game_session/PlayerSession.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace gamenet::net {
class EventLoop;
}

namespace gamenet::game_session {

enum class DuplicateLoginPolicy {
    ReplaceExisting,
    RejectNew,
};

enum class AuthenticateStatus {
    Created,
    Existing,
    Rebound,
    Rejected,
};

struct AuthenticateResult {
    AuthenticateStatus status{AuthenticateStatus::Rejected};
    std::shared_ptr<PlayerSession> session;
};

class SessionManager {
public:
    using Clock = PlayerSession::Clock;
    using AuthenticateCallback = std::function<void(AuthenticateResult)>;

    struct Options {
        DuplicateLoginPolicy duplicateLogin{DuplicateLoginPolicy::ReplaceExisting};
        Clock::duration idleTimeout{std::chrono::minutes(2)};
    };

    explicit SessionManager(gamenet::net::EventLoop* ownerLoop);
    SessionManager(gamenet::net::EventLoop* ownerLoop, Options options);

    gamenet::net::EventLoop* ownerLoop() const noexcept;

    AuthenticateResult authenticate(
        PlayerId playerId,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        Clock::time_point now = Clock::now());
    bool offline(gamenet::transport::TransportSessionId transportId);
    bool heartbeat(
        gamenet::transport::TransportSessionId transportId,
        Clock::time_point now = Clock::now());
    std::size_t expireIdle(Clock::time_point now = Clock::now());

    std::shared_ptr<PlayerSession> findByPlayer(const PlayerId& playerId) const;
    std::shared_ptr<PlayerSession> findByTransport(
        gamenet::transport::TransportSessionId transportId) const;
    std::size_t size() const;

    void postAuthenticate(
        PlayerId playerId,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        AuthenticateCallback callback = {});
    void postOffline(gamenet::transport::TransportSessionId transportId);
    void postHeartbeat(gamenet::transport::TransportSessionId transportId);

private:
    void assertOwner() const;
    void closeEndpoint(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        gamenet::transport::CloseReason reason);
    void eraseSession(const std::shared_ptr<PlayerSession>& session);

    gamenet::net::EventLoop* ownerLoop_;
    Options options_;
    SessionId nextSessionId_{1};
    std::unordered_map<PlayerId, std::shared_ptr<PlayerSession>> byPlayer_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PlayerSession>> byTransport_;
};

}  // namespace gamenet::game_session
