#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/game_session/PlayerSession.h"
#include "gamenet/transport/TransportEndpoint.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class OrderedEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    OrderedEndpoint(
        std::uint64_t id,
        gamenet::net::EventLoopExecutor executor,
        std::atomic<std::size_t>* sends,
        std::size_t expectedSends,
        std::promise<void>* completed,
        bool slow)
        : id_{id},
          executor_(std::move(executor)),
          sends_(sends),
          expectedSends_(expectedSends),
          completed_(completed),
          slow_(slow) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override { return executor_; }
    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        if (slow_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard lock(mutex_);
            received_.emplace_back(bytes);
        }
        if (sends_->fetch_add(1) + 1 == expectedSends_) completed_->set_value();
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return true; }
    std::vector<std::string> received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoopExecutor executor_;
    std::atomic<std::size_t>* sends_;
    std::size_t expectedSends_;
    std::promise<void>* completed_;
    bool slow_;
    mutable std::mutex mutex_;
    std::vector<std::string> received_;
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    constexpr std::size_t endpointCount = 1024;
    constexpr std::size_t messageCount = 12;
    constexpr std::size_t expectedSends = endpointCount * messageCount;

    gamenet::net::EventLoop managementLoop;
    gamenet::net::EventLoopThread firstThread;
    gamenet::net::EventLoopThread secondThread;
    auto* firstLoop = firstThread.startLoop();
    auto* secondLoop = secondThread.startLoop();

    std::atomic<std::size_t> sends{0};
    std::promise<void> completed;
    auto completedFuture = completed.get_future();
    std::vector<std::shared_ptr<OrderedEndpoint>> endpoints;
    std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> sessions;
    endpoints.reserve(endpointCount);
    sessions.reserve(endpointCount);
    for (std::size_t index = 0; index < endpointCount; ++index) {
        auto endpoint = std::make_shared<OrderedEndpoint>(
            index + 1,
            (index % 2 == 0 ? firstLoop : secondLoop)->executor(),
            &sends,
            expectedSends,
            &completed,
            index == 0);
        auto session = std::make_shared<gamenet::game_session::PlayerSession>(
            index + 1,
            "fanout-player-" + std::to_string(index),
            endpoint,
            gamenet::game_session::PlayerSession::Clock::now());
        session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
        endpoints.push_back(std::move(endpoint));
        sessions.push_back(std::move(session));
    }

    gamenet::broadcast::BroadcastRouter router(
        &managementLoop,
        {.softFanout = endpointCount,
         .hardFanout = endpointCount,
         .softBytes = 16 * endpointCount,
         .hardBytes = 16 * endpointCount});
    gamenet::broadcast::BroadcastDispatcher dispatcher(
        {.maxEndpointsPerTask = 32, .maxBytesPerTask = 512});
    for (std::size_t sequence = 0; sequence < messageCount; ++sequence) {
        const auto payload = std::make_shared<const std::string>(std::to_string(sequence));
        auto plan = router.route(payload, sessions);
        const auto summary = dispatcher.dispatch(std::move(plan));
        GAMENET_TEST_ASSERT(summary.scheduledEndpoints == endpointCount);
        GAMENET_TEST_ASSERT(summary.scheduledTasks == endpointCount / 32);
    }

    gamenet::test::waitUntilReady(completedFuture, 5s, "large fanout did not drain");
    GAMENET_TEST_ASSERT(sends.load() == expectedSends);
    for (const auto& endpoint : endpoints) {
        const auto received = endpoint->received();
        GAMENET_TEST_ASSERT(received.size() == messageCount);
        for (std::size_t sequence = 0; sequence < messageCount; ++sequence) {
            GAMENET_TEST_ASSERT(received[sequence] == std::to_string(sequence));
        }
    }

    firstThread.stop();
    secondThread.stop();
}
