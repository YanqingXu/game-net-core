#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class FakeEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    FakeEndpoint(std::uint64_t id, gamenet::net::EventLoop* loop) : id_{id}, loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        if (!loop_->isInLoopThread()) return gamenet::transport::EndpointResult::WrongThread;
        sent_.emplace_back(bytes);
        return open_ ? gamenet::transport::EndpointResult::Accepted
                     : gamenet::transport::EndpointResult::Closed;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason reason) override {
        if (!loop_->isInLoopThread()) return gamenet::transport::EndpointResult::WrongThread;
        open_ = false;
        closeReason_ = reason;
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_; }
    gamenet::transport::CloseReason closeReason() const noexcept { return closeReason_; }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    bool open_{true};
    gamenet::transport::CloseReason closeReason_{gamenet::transport::CloseReason::Normal};
    std::vector<std::string> sent_;
};

using PublicPlayerLookup = decltype(
    std::declval<const gamenet::game_session::SessionManager&>().findByPlayer(
        std::declval<const gamenet::game_session::PlayerId&>()));
using PublicTransportLookup = decltype(
    std::declval<const gamenet::game_session::SessionManager&>().findByTransport(
        std::declval<gamenet::transport::TransportSessionId>()));
using PublicAuthenticateSession =
    decltype(gamenet::game_session::AuthenticateResult{}.session);

template <typename Pointer>
concept CanMarkOnline = requires(
    Pointer pointer,
    gamenet::game_session::PlayerSession::Clock::time_point now) {
    pointer->markOnline(now);
};

template <typename Pointer>
concept CanMarkOffline = requires(Pointer pointer) {
    pointer->markOffline();
};

template <typename Pointer>
concept CanRebind = requires(
    Pointer pointer,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    gamenet::game_session::PlayerSession::Clock::time_point now) {
    pointer->rebind(std::move(endpoint), now);
};

template <typename Pointer>
concept CanHeartbeat = requires(
    Pointer pointer,
    gamenet::game_session::PlayerSession::Clock::time_point now) {
    pointer->heartbeat(now);
};

static_assert(std::is_same_v<
              PublicPlayerLookup,
              std::shared_ptr<const gamenet::game_session::PlayerSession>>);
static_assert(std::is_same_v<PublicTransportLookup, PublicPlayerLookup>);
static_assert(std::is_same_v<PublicAuthenticateSession, PublicPlayerLookup>);
static_assert(std::is_const_v<typename PublicPlayerLookup::element_type>);
static_assert(!std::is_constructible_v<
              gamenet::game_session::PlayerSession,
              gamenet::game_session::SessionId,
              gamenet::game_session::PlayerId,
              std::shared_ptr<gamenet::transport::TransportEndpoint>,
              gamenet::game_session::SessionBindingGeneration,
              gamenet::game_session::PlayerSession::Clock::time_point>);
static_assert(!CanMarkOnline<PublicPlayerLookup>);
static_assert(!CanMarkOffline<PublicPlayerLookup>);
static_assert(!CanRebind<PublicPlayerLookup>);
static_assert(!CanHeartbeat<PublicPlayerLookup>);

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using gamenet::game_session::AuthenticateStatus;

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager manager(
            &loop,
            {.duplicateLogin = gamenet::game_session::DuplicateLoginPolicy::ReplaceExisting,
             .idleTimeout = 10ms});
        auto first = std::make_shared<FakeEndpoint>(1, &loop);
        auto second = std::make_shared<FakeEndpoint>(2, &loop);

        const auto start = gamenet::game_session::PlayerSession::Clock::now();
        auto created = manager.authenticate("player", first, start);
        GAMENET_TEST_ASSERT(created.status == AuthenticateStatus::Created);
        GAMENET_TEST_ASSERT(created.session->state() == gamenet::game_session::SessionState::Online);
        GAMENET_TEST_ASSERT(manager.size() == 1);

        auto rebound = manager.authenticate("player", second, start + 1ms);
        GAMENET_TEST_ASSERT(rebound.status == AuthenticateStatus::Rebound);
        GAMENET_TEST_ASSERT(rebound.session == created.session);
        GAMENET_TEST_ASSERT(rebound.session->transportId().value == 2);
        GAMENET_TEST_ASSERT(!manager.offline({1}));
        GAMENET_TEST_ASSERT(manager.findByPlayer("player") == rebound.session);
        GAMENET_TEST_ASSERT(manager.heartbeat({2}, start + 8ms));
        GAMENET_TEST_ASSERT(manager.expireIdle(start + 15ms) == 0);
        GAMENET_TEST_ASSERT(manager.expireIdle(start + 19ms) == 1);
        GAMENET_TEST_ASSERT(manager.size() == 0);

        bool asyncCallback = false;
        auto third = std::make_shared<FakeEndpoint>(3, &loop);
        std::thread ioThread([&] {
            manager.postAuthenticate("async-player", third, [&](auto result) {
                GAMENET_TEST_ASSERT(loop.isInLoopThread());
                GAMENET_TEST_ASSERT(result.status == AuthenticateStatus::Created);
                asyncCallback = true;
                loop.quit();
            });
        });
        ioThread.join();
        loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("session async submit timed out"); });
        loop.loop();

        GAMENET_TEST_ASSERT(asyncCallback);
        GAMENET_TEST_ASSERT(!first->isOpen());
        GAMENET_TEST_ASSERT(first->closeReason() == gamenet::transport::CloseReason::Replaced);
        GAMENET_TEST_ASSERT(!second->isOpen());
        GAMENET_TEST_ASSERT(second->closeReason() == gamenet::transport::CloseReason::IdleTimeout);
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager manager(
            &loop,
            {
                .duplicateLogin =
                    gamenet::game_session::DuplicateLoginPolicy::ReplaceExisting,
                .idleTimeout = 10ms,
                .idleDeadlineResolution = 1ms,
                .maxIdleExpirationsPerAdvance = 3,
            });
        const auto start =
            gamenet::game_session::SessionManager::Clock::now();
        for (std::uint64_t index = 0; index < 10; ++index) {
            auto endpoint =
                std::make_shared<FakeEndpoint>(100 + index, &loop);
            GAMENET_TEST_ASSERT(
                manager.authenticate(
                    "bucket-player-" + std::to_string(index),
                    endpoint,
                    start).status == AuthenticateStatus::Created);
        }

        // session-idle-deadline-budget-contract: a same-bucket population is
        // removed in exact bounded batches without scanning the player index.
        std::size_t expired = 0;
        std::size_t batches = 0;
        bool readyRemaining = false;
        do {
            const auto result = manager.expireIdleBatch(start + 20ms);
            expired += result.expired;
            readyRemaining = result.readyRemaining;
            ++batches;
        } while (readyRemaining);
        GAMENET_TEST_ASSERT(expired == 10);
        GAMENET_TEST_ASSERT(batches == 4);
        GAMENET_TEST_ASSERT(manager.size() == 0);
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager rejectManager(
            &loop,
            {.duplicateLogin = gamenet::game_session::DuplicateLoginPolicy::RejectNew});
        auto acceptedEndpoint = std::make_shared<FakeEndpoint>(10, &loop);
        auto rejectedEndpoint = std::make_shared<FakeEndpoint>(11, &loop);
        GAMENET_TEST_ASSERT(
            rejectManager.authenticate("same-player", acceptedEndpoint).status ==
            AuthenticateStatus::Created);
        GAMENET_TEST_ASSERT(
            rejectManager.authenticate("same-player", rejectedEndpoint).status ==
            AuthenticateStatus::Rejected);
        GAMENET_TEST_ASSERT(
            rejectManager.findByPlayer("same-player")->transportId().value == 10);

        gamenet::game_session::SessionManager collisionManager(&loop);
        auto collisionFirst = std::make_shared<FakeEndpoint>(20, &loop);
        auto collisionSecond = std::make_shared<FakeEndpoint>(20, &loop);
        auto samePlayerDifferentEndpoint = std::make_shared<FakeEndpoint>(20, &loop);
        auto secondOwnerEndpoint = std::make_shared<FakeEndpoint>(21, &loop);
        auto rebindCollisionEndpoint = std::make_shared<FakeEndpoint>(21, &loop);

        const auto firstCreated = collisionManager.authenticate("first-player", collisionFirst);
        GAMENET_TEST_ASSERT(firstCreated.status == AuthenticateStatus::Created);
        GAMENET_TEST_ASSERT(
            collisionManager.authenticate("first-player", collisionFirst).status ==
            AuthenticateStatus::Existing);

        const auto differentPlayerCollision =
            collisionManager.authenticate("second-player", collisionSecond);
        GAMENET_TEST_ASSERT(differentPlayerCollision.status == AuthenticateStatus::Rejected);
        GAMENET_TEST_ASSERT(!differentPlayerCollision.session);

        const auto samePlayerCollision =
            collisionManager.authenticate("first-player", samePlayerDifferentEndpoint);
        GAMENET_TEST_ASSERT(samePlayerCollision.status == AuthenticateStatus::Rejected);
        GAMENET_TEST_ASSERT(!samePlayerCollision.session);

        GAMENET_TEST_ASSERT(
            collisionManager.authenticate("second-player", secondOwnerEndpoint).status ==
            AuthenticateStatus::Created);
        const auto rebindCollision =
            collisionManager.authenticate("first-player", rebindCollisionEndpoint);
        GAMENET_TEST_ASSERT(rebindCollision.status == AuthenticateStatus::Rejected);
        GAMENET_TEST_ASSERT(!rebindCollision.session);

        loop.runAfter(1ms, [&] {
            loop.queueInLoop([&] {
                GAMENET_TEST_ASSERT(collisionFirst->isOpen());
                GAMENET_TEST_ASSERT(secondOwnerEndpoint->isOpen());
                for (const auto& endpoint : {
                         collisionSecond,
                         samePlayerDifferentEndpoint,
                         rebindCollisionEndpoint}) {
                    GAMENET_TEST_ASSERT(!endpoint->isOpen());
                    GAMENET_TEST_ASSERT(
                        endpoint->closeReason() ==
                        gamenet::transport::CloseReason::ProtocolError);
                }
                GAMENET_TEST_ASSERT(collisionManager.size() == 2);
                GAMENET_TEST_ASSERT(
                    collisionManager.findByPlayer("first-player") == firstCreated.session);
                GAMENET_TEST_ASSERT(
                    collisionManager.findByTransport({20})->playerId() == "first-player");
                GAMENET_TEST_ASSERT(
                    collisionManager.findByTransport({21})->playerId() == "second-player");
                loop.quit();
            });
        });
        loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("session collision close timed out"); });
        loop.loop();
    }
}
