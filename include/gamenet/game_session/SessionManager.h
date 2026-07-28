#pragma once

#include "gamenet/core/DispatchResult.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/game_session/PlayerSession.h"

#include <chrono>
#include <atomic>
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
    std::shared_ptr<const PlayerSession> session;
    gamenet::DispatchResult dispatch{gamenet::DispatchResult::PolicyRejected};
};

class SessionManager {
public:
    using Clock = PlayerSession::Clock;
    using AuthenticateCallback = std::function<void(AuthenticateResult)>;
    using MutationCallback = std::function<void(gamenet::DispatchResult)>;

    struct Options {
        DuplicateLoginPolicy duplicateLogin{DuplicateLoginPolicy::ReplaceExisting};
        Clock::duration idleTimeout{std::chrono::minutes(2)};
    };

    explicit SessionManager(gamenet::net::EventLoop* ownerLoop);
    SessionManager(gamenet::net::EventLoop* ownerLoop, Options options);
    ~SessionManager();

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

    // Returned const views remain management-loop-affine. Constness prevents
    // index-breaking mutation; it does not make PlayerSession a cross-thread snapshot.
    std::shared_ptr<const PlayerSession> findByPlayer(const PlayerId& playerId) const;
    std::shared_ptr<const PlayerSession> findByTransport(
        gamenet::transport::TransportSessionId transportId) const;
    std::size_t size() const;
    // Owner-loop-only, one-shot revocation. Clears both indexes, marks indexed
    // sessions Offline, and requests GoingAway close on endpoint owners.
    void shutdown();

    gamenet::DispatchResult postAuthenticate(
        PlayerId playerId,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        AuthenticateCallback callback = {});
    gamenet::DispatchResult postOffline(
        gamenet::transport::TransportSessionId transportId,
        MutationCallback callback = {});
    gamenet::DispatchResult postHeartbeat(
        gamenet::transport::TransportSessionId transportId,
        MutationCallback callback = {});

private:
    struct LifetimeState {
        bool active() const noexcept {
            return terminal.load(std::memory_order_acquire) ==
                gamenet::DispatchResult::Accepted;
        }
        gamenet::DispatchResult result() const noexcept {
            return terminal.load(std::memory_order_acquire);
        }
        void revoke(gamenet::DispatchResult result) noexcept {
            auto expected = gamenet::DispatchResult::Accepted;
            (void)terminal.compare_exchange_strong(
                expected, result, std::memory_order_acq_rel);
        }

        std::atomic<gamenet::DispatchResult> terminal{
            gamenet::DispatchResult::Accepted};
    };

    enum class LifecycleState {
        Running,
        Shutdown,
    };

    void assertOwner() const;
    void closeEndpoint(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        gamenet::transport::CloseReason reason);
    std::shared_ptr<PlayerSession> findMutableByTransport(
        gamenet::transport::TransportSessionId transportId) const;
    void eraseSession(const std::shared_ptr<PlayerSession>& session);

    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::EventLoopExecutor ownerExecutor_;
    std::shared_ptr<LifetimeState> lifetimeState_;
    LifecycleState lifecycleState_{LifecycleState::Running};
    Options options_;
    SessionId nextSessionId_{1};
    SessionBindingGeneration nextBindingGeneration_{1};
    std::unordered_map<PlayerId, std::shared_ptr<PlayerSession>> byPlayer_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PlayerSession>> byTransport_;
};

}  // namespace gamenet::game_session
