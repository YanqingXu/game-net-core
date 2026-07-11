#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>

namespace {

class LifetimeEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    LifetimeEndpoint(std::uint64_t id, gamenet::net::EventLoop* loop) : id_{id}, loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        return loop_->isInLoopThread() ? gamenet::transport::EndpointResult::Accepted
                                       : gamenet::transport::EndpointResult::WrongThread;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false, std::memory_order_relaxed);
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_.load(std::memory_order_relaxed); }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    std::atomic<bool> open_{true};
};

class CrossLoopEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    CrossLoopEndpoint(std::uint64_t id, gamenet::net::EventLoop* ownerLoop)
        : id_{id}, ownerLoop_(ownerLoop), ownerExecutor_(ownerLoop->executor()) {
        GAMENET_TEST_ASSERT(ownerLoop_->isInLoopThread());
    }

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return ownerExecutor_;
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        GAMENET_TEST_ASSERT(ownerLoop_->isInLoopThread());
        return open_.load(std::memory_order_acquire)
                   ? gamenet::transport::EndpointResult::Accepted
                   : gamenet::transport::EndpointResult::Closed;
    }
    gamenet::transport::EndpointResult close(
        gamenet::transport::CloseReason reason) override {
        GAMENET_TEST_ASSERT(ownerLoop_->isInLoopThread());
        GAMENET_TEST_ASSERT(closeCount_.fetch_add(1, std::memory_order_acq_rel) == 0);
        closeThread_ = std::this_thread::get_id();
        closeReason_.store(reason, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        closed_.release();
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_.load(std::memory_order_acquire); }

    bool waitUntilClosed(std::chrono::milliseconds timeout) {
        return closed_.try_acquire_for(timeout);
    }
    int closeCount() const noexcept { return closeCount_.load(std::memory_order_acquire); }
    gamenet::transport::CloseReason closeReason() const noexcept {
        return closeReason_.load(std::memory_order_acquire);
    }
    std::thread::id closeThread() const noexcept { return closeThread_; }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* ownerLoop_;
    gamenet::net::EventLoopExecutor ownerExecutor_;
    std::atomic<bool> open_{true};
    std::atomic<int> closeCount_{0};
    std::atomic<gamenet::transport::CloseReason> closeReason_{
        gamenet::transport::CloseReason::Normal};
    std::thread::id closeThread_;
    std::binary_semaphore closed_{0};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    std::atomic<bool> destroyedCallbackRan{false};
    {
        gamenet::net::EventLoop loop;
        auto endpoint = std::make_shared<LifetimeEndpoint>(1, &loop);
        auto manager = std::make_unique<gamenet::game_session::SessionManager>(&loop);
        std::thread submitter([&] {
            manager->postAuthenticate("destroyed", endpoint, [&](auto) {
                destroyedCallbackRan.store(true, std::memory_order_relaxed);
            });
            manager->postHeartbeat({1});
            manager->postOffline({1});
        });
        submitter.join();
        manager.reset();

        loop.runAfter(2ms, [&] { loop.quit(); });
        loop.loop();
    }
    GAMENET_TEST_ASSERT(!destroyedCallbackRan.load(std::memory_order_relaxed));

    {
        gamenet::net::EventLoop loop;
        auto manager = std::make_unique<gamenet::game_session::SessionManager>(&loop);
        auto activeEndpoint = std::make_shared<LifetimeEndpoint>(20, &loop);
        auto destroyingEndpoint = std::make_shared<LifetimeEndpoint>(21, &loop);
        auto revokedEndpoint = std::make_shared<LifetimeEndpoint>(22, &loop);
        const auto initialActivity =
            gamenet::game_session::PlayerSession::Clock::now() - std::chrono::seconds(1);
        const auto active = manager->authenticate(
            "callback-destroy-active", activeEndpoint, initialActivity);
        GAMENET_TEST_ASSERT(active.session);

        bool destroyingCallbackRan = false;
        bool revokedCallbackRan = false;
        manager->postAuthenticate(
            "callback-destroys-manager",
            destroyingEndpoint,
            [&](auto result) {
                GAMENET_TEST_ASSERT(
                    result.status == gamenet::game_session::AuthenticateStatus::Created);
                destroyingCallbackRan = true;
                manager.reset();
            });
        manager->postAuthenticate("revoked-authentication", revokedEndpoint, [&](auto) {
            revokedCallbackRan = true;
        });
        manager->postHeartbeat({20});
        manager->postOffline({20});

        loop.quit();
        loop.loop();

        GAMENET_TEST_ASSERT(destroyingCallbackRan);
        GAMENET_TEST_ASSERT(!revokedCallbackRan);
        GAMENET_TEST_ASSERT(!manager);
        GAMENET_TEST_ASSERT(
            active.session->state() == gamenet::game_session::SessionState::Online);
        GAMENET_TEST_ASSERT(active.session->lastActivity() == initialActivity);
    }

    {
        gamenet::net::EventLoopThread endpointThread;
        auto* endpointLoop = endpointThread.startLoop();
        gamenet::net::EventLoop managementLoop;
        const auto managementThread = std::this_thread::get_id();
        gamenet::game_session::SessionManager manager(&managementLoop);
        std::promise<std::shared_ptr<CrossLoopEndpoint>> endpointPromise;
        auto endpointFuture = endpointPromise.get_future();
        endpointLoop->queueInLoop([endpointLoop, &endpointPromise] {
            endpointPromise.set_value(std::make_shared<CrossLoopEndpoint>(10, endpointLoop));
        });
        GAMENET_TEST_ASSERT(endpointFuture.wait_for(2s) == std::future_status::ready);
        auto activeEndpoint = endpointFuture.get();
        auto queuedEndpoint = std::make_shared<LifetimeEndpoint>(11, &managementLoop);
        auto afterShutdownEndpoint =
            std::make_shared<LifetimeEndpoint>(12, &managementLoop);
        const auto active = manager.authenticate("shutdown-active", activeEndpoint);
        GAMENET_TEST_ASSERT(active.status == gamenet::game_session::AuthenticateStatus::Created);
        GAMENET_TEST_ASSERT(active.session);

        std::atomic<int> authenticationCallbacks{0};
        std::thread queuedBeforeShutdown([&] {
            manager.postHeartbeat({10});
            manager.postOffline({10});
            manager.postAuthenticate("queued-before-shutdown", queuedEndpoint, [&](auto) {
                authenticationCallbacks.fetch_add(1, std::memory_order_relaxed);
            });
        });
        queuedBeforeShutdown.join();

        manager.shutdown();
        manager.shutdown();
        GAMENET_TEST_ASSERT(
            active.session->state() == gamenet::game_session::SessionState::Offline);
        GAMENET_TEST_ASSERT(manager.size() == 0);
        GAMENET_TEST_ASSERT(!manager.findByPlayer("shutdown-active"));
        GAMENET_TEST_ASSERT(!manager.findByTransport({10}));
        GAMENET_TEST_ASSERT(
            manager.authenticate("after-shutdown", afterShutdownEndpoint).status ==
            gamenet::game_session::AuthenticateStatus::Rejected);
        GAMENET_TEST_ASSERT(!manager.offline({10}));
        GAMENET_TEST_ASSERT(!manager.heartbeat({10}));
        GAMENET_TEST_ASSERT(manager.expireIdle() == 0);

        std::thread submittedAfterShutdown([&] {
            manager.postAuthenticate("submitted-after-shutdown", afterShutdownEndpoint, [&](auto) {
                authenticationCallbacks.fetch_add(1, std::memory_order_relaxed);
            });
            manager.postHeartbeat({10});
            manager.postOffline({10});
        });
        submittedAfterShutdown.join();

        GAMENET_TEST_ASSERT(activeEndpoint->waitUntilClosed(2s));
        GAMENET_TEST_ASSERT(activeEndpoint->closeCount() == 1);
        GAMENET_TEST_ASSERT(
            activeEndpoint->closeReason() == gamenet::transport::CloseReason::GoingAway);
        GAMENET_TEST_ASSERT(activeEndpoint->closeThread() != managementThread);
        GAMENET_TEST_ASSERT(!activeEndpoint->isOpen());

        managementLoop.quit();
        managementLoop.loop();
        GAMENET_TEST_ASSERT(authenticationCallbacks.load(std::memory_order_relaxed) == 0);
        GAMENET_TEST_ASSERT(manager.size() == 0);
        GAMENET_TEST_ASSERT(!manager.findByPlayer("queued-before-shutdown"));
        GAMENET_TEST_ASSERT(!manager.findByPlayer("submitted-after-shutdown"));
        GAMENET_TEST_ASSERT(afterShutdownEndpoint->isOpen());
        GAMENET_TEST_ASSERT(activeEndpoint->closeCount() == 1);
    }

    {
        gamenet::net::EventLoop raceLoop;
        gamenet::game_session::SessionManager raceManager(&raceLoop);
        auto raceEndpoint = std::make_shared<LifetimeEndpoint>(4, &raceLoop);
        const auto initialActivity =
            gamenet::game_session::PlayerSession::Clock::now() - std::chrono::seconds(1);
        const auto created =
            raceManager.authenticate("racing-posts", raceEndpoint, initialActivity);
        GAMENET_TEST_ASSERT(created.session);
        GAMENET_TEST_ASSERT(created.session->lastActivity() == initialActivity);

        std::latch start{1};
        std::latch producersReady{2};
        std::atomic<bool> keepSubmitting{true};
        std::atomic<bool> allowOffline{false};
        std::atomic<int> activeProducers{2};
        std::atomic<int> heartbeatPosts{0};
        std::atomic<int> offlinePosts{0};
        bool heartbeatAdvanceObserved = false;
        bool submitDrainOverlapObserved = false;
        int producerDrainSentinels = 0;
        const auto verifyAfterProducerDrain = [&] {
            ++producerDrainSentinels;
            if (producerDrainSentinels != 2) return;
            GAMENET_TEST_ASSERT(heartbeatAdvanceObserved);
            GAMENET_TEST_ASSERT(submitDrainOverlapObserved);
            GAMENET_TEST_ASSERT(heartbeatPosts.load(std::memory_order_acquire) > 0);
            GAMENET_TEST_ASSERT(offlinePosts.load(std::memory_order_acquire) > 0);
            GAMENET_TEST_ASSERT(
                created.session->state() == gamenet::game_session::SessionState::Offline);
            GAMENET_TEST_ASSERT(raceManager.size() == 0);
            GAMENET_TEST_ASSERT(!raceManager.findByPlayer("racing-posts"));
            GAMENET_TEST_ASSERT(!raceManager.findByTransport({4}));
            raceLoop.quit();
        };
        std::thread heartbeatProducer([&] {
            producersReady.count_down();
            start.wait();
            while (keepSubmitting.load(std::memory_order_acquire)) {
                heartbeatPosts.fetch_add(1, std::memory_order_release);
                raceManager.postHeartbeat({4});
                std::this_thread::sleep_for(100us);
            }
            activeProducers.fetch_sub(1, std::memory_order_release);
            raceLoop.queueInLoop(verifyAfterProducerDrain);
        });
        std::thread offlineProducer([&] {
            producersReady.count_down();
            start.wait();
            while (!allowOffline.load(std::memory_order_acquire) &&
                   keepSubmitting.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (keepSubmitting.load(std::memory_order_acquire)) {
                offlinePosts.fetch_add(1, std::memory_order_release);
                raceManager.postOffline({4});
                std::this_thread::sleep_for(100us);
            }
            activeProducers.fetch_sub(1, std::memory_order_release);
            raceLoop.queueInLoop(verifyAfterProducerDrain);
        });
        producersReady.wait();
        start.count_down();
        raceLoop.runEvery(1ms, [&] {
            if (submitDrainOverlapObserved) {
                return;
            }
            if (!heartbeatAdvanceObserved) {
                if (created.session->lastActivity() == initialActivity) return;
                GAMENET_TEST_ASSERT(
                    created.session->lastActivity() > initialActivity);
                GAMENET_TEST_ASSERT(heartbeatPosts.load(std::memory_order_acquire) > 0);
                GAMENET_TEST_ASSERT(activeProducers.load(std::memory_order_acquire) == 2);
                heartbeatAdvanceObserved = true;
                allowOffline.store(true, std::memory_order_release);
                return;
            }
            if (created.session->state() != gamenet::game_session::SessionState::Offline) return;
            GAMENET_TEST_ASSERT(offlinePosts.load(std::memory_order_acquire) > 0);
            GAMENET_TEST_ASSERT(activeProducers.load(std::memory_order_acquire) == 2);
            submitDrainOverlapObserved = true;
            keepSubmitting.store(false, std::memory_order_release);
        });
        raceLoop.runAfter(2s, [&] { GAMENET_TEST_FAIL("session post race timed out"); });
        raceLoop.loop();
        heartbeatProducer.join();
        offlineProducer.join();
    }

    bool reentered = false;
    {
        gamenet::net::EventLoop reentryLoop;
        gamenet::game_session::SessionManager reentryManager(&reentryLoop);
        auto reentryEndpoint = std::make_shared<LifetimeEndpoint>(2, &reentryLoop);
        auto reboundEndpoint = std::make_shared<LifetimeEndpoint>(3, &reentryLoop);
        std::thread submitter([&] {
            reentryManager.postAuthenticate("reentry", reentryEndpoint, [&](auto result) {
                GAMENET_TEST_ASSERT(reentryLoop.isInLoopThread());
                GAMENET_TEST_ASSERT(result.session);
                GAMENET_TEST_ASSERT(
                    reentryManager.findByPlayer("reentry") == result.session);
                GAMENET_TEST_ASSERT(reentryManager.heartbeat({2}));

                const auto rebound =
                    reentryManager.authenticate("reentry", reboundEndpoint);
                GAMENET_TEST_ASSERT(
                    rebound.status == gamenet::game_session::AuthenticateStatus::Rebound);
                GAMENET_TEST_ASSERT(rebound.session == result.session);
                GAMENET_TEST_ASSERT(!reentryManager.findByTransport({2}));
                GAMENET_TEST_ASSERT(
                    reentryManager.findByTransport({3}) == rebound.session);
                GAMENET_TEST_ASSERT(reentryEndpoint->isOpen());

                reentryLoop.queueInLoop([&] {
                    GAMENET_TEST_ASSERT(!reentryEndpoint->isOpen());
                    GAMENET_TEST_ASSERT(reboundEndpoint->isOpen());
                    GAMENET_TEST_ASSERT(reentryManager.offline({3}));
                    GAMENET_TEST_ASSERT(reentryManager.size() == 0);
                    reentered = true;
                    reentryLoop.quit();
                });
            });
        });
        submitter.join();
        reentryLoop.runAfter(2s, [&] { GAMENET_TEST_FAIL("session re-entry timed out"); });
        reentryLoop.loop();
    }
    GAMENET_TEST_ASSERT(reentered);
}
