#include "gamenet/core/DispatchResult.h"
#include "gamenet/game_session/SessionManager.h"

#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"

#include <atomic>
#include <memory>
#include <string_view>
#include <thread>

namespace {

class DispatchEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    DispatchEndpoint(std::uint64_t id, gamenet::net::EventLoop* loop)
        : id_{id}, loop_(loop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        open_.store(false, std::memory_order_release);
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::DispatchResult requestClose(
        gamenet::transport::CloseReason reason) noexcept override {
        const auto post = loop_->executor().post([this, reason] {
            (void)close(reason);
        });
        return gamenet::dispatchResult(post);
    }
    bool isOpen() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    std::atomic<bool> open_{true};
};

}  // namespace

int main() {
    {
        gamenet::net::EventLoop loop({
            .maxPendingFunctors = 1,
            .reservedPendingFunctors = 0,
            .maxFunctorsPerIteration = 1,
        });
        gamenet::game_session::SessionManager manager(&loop);
        auto endpoint = std::make_shared<DispatchEndpoint>(1, &loop);

        GAMENET_TEST_ASSERT(
            loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);

        gamenet::DispatchResult authenticateResult = gamenet::DispatchResult::Accepted;
        gamenet::DispatchResult offlineResult = gamenet::DispatchResult::Accepted;
        gamenet::DispatchResult heartbeatResult = gamenet::DispatchResult::Accepted;
        std::thread producer([&] {
            authenticateResult = manager.postAuthenticate("full", endpoint);
            offlineResult = manager.postOffline({1});
            heartbeatResult = manager.postHeartbeat({1});
        });
        producer.join();

        GAMENET_TEST_ASSERT(authenticateResult == gamenet::DispatchResult::QueueFull);
        GAMENET_TEST_ASSERT(offlineResult == gamenet::DispatchResult::QueueFull);
        GAMENET_TEST_ASSERT(heartbeatResult == gamenet::DispatchResult::QueueFull);
    }

    {
        gamenet::net::EventLoop loop;
        gamenet::game_session::SessionManager manager(&loop);
        auto endpoint = std::make_shared<DispatchEndpoint>(2, &loop);
        gamenet::game_session::AuthenticateResult terminal;
        std::atomic<int> callbackCount{0};
        std::atomic<int> offlineCallbackCount{0};
        std::atomic<int> heartbeatCallbackCount{0};
        gamenet::DispatchResult offlineTerminal = gamenet::DispatchResult::Accepted;
        gamenet::DispatchResult heartbeatTerminal = gamenet::DispatchResult::Accepted;

        GAMENET_TEST_ASSERT(
            manager.postAuthenticate(
                "linearized-shutdown",
                endpoint,
                [&](gamenet::game_session::AuthenticateResult result) {
                    terminal = std::move(result);
                    callbackCount.fetch_add(1, std::memory_order_relaxed);
                }) == gamenet::DispatchResult::Accepted);
        GAMENET_TEST_ASSERT(
            manager.postOffline({2}, [&](gamenet::DispatchResult result) {
                offlineTerminal = result;
                offlineCallbackCount.fetch_add(1, std::memory_order_relaxed);
            }) == gamenet::DispatchResult::Accepted);
        GAMENET_TEST_ASSERT(
            manager.postHeartbeat({2}, [&](gamenet::DispatchResult result) {
                heartbeatTerminal = result;
                heartbeatCallbackCount.fetch_add(1, std::memory_order_relaxed);
            }) == gamenet::DispatchResult::Accepted);

        manager.shutdown();
        GAMENET_TEST_ASSERT(
            manager.postAuthenticate("after-shutdown", endpoint) ==
            gamenet::DispatchResult::Shutdown);
        GAMENET_TEST_ASSERT(
            manager.postOffline({2}) == gamenet::DispatchResult::Shutdown);
        GAMENET_TEST_ASSERT(
            manager.postHeartbeat({2}) == gamenet::DispatchResult::Shutdown);

        loop.quit();
        loop.loop();
        GAMENET_TEST_ASSERT(callbackCount.load(std::memory_order_relaxed) == 1);
        GAMENET_TEST_ASSERT(
            offlineCallbackCount.load(std::memory_order_relaxed) == 1);
        GAMENET_TEST_ASSERT(
            heartbeatCallbackCount.load(std::memory_order_relaxed) == 1);
        GAMENET_TEST_ASSERT(terminal.dispatch == gamenet::DispatchResult::Shutdown);
        GAMENET_TEST_ASSERT(offlineTerminal == gamenet::DispatchResult::Shutdown);
        GAMENET_TEST_ASSERT(heartbeatTerminal == gamenet::DispatchResult::Shutdown);
        GAMENET_TEST_ASSERT(!terminal.session);
    }
}
