#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gamenet::examples {

class GameServerPipelineM2TestPeer {
public:
    static void injectIoBytes(
        GameServerPipeline& pipeline,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        std::string bytes,
        std::function<void(std::vector<std::string>)> deliver) {
        pipeline.injectIoBytesForTesting(
            std::move(endpoint), std::move(bytes), std::move(deliver));
    }

    static void addOnlineConnection(
        GameServerPipeline& pipeline,
        std::string name,
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        std::string player) {
        pipeline.onConnected(name, endpoint);
        const auto authenticated =
            pipeline.sessions_.authenticate(std::move(player), endpoint);
        GAMENET_TEST_ASSERT(authenticated.session);
        auto& state = pipeline.connections_.at(name);
        state.sessionId = authenticated.session->sessionId();
        state.binding = authenticated.session->binding();
        state.authentication = GameServerPipeline::AuthenticationState::Online;
    }

    static void addConnection(
        GameServerPipeline& pipeline,
        std::string name,
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint) {
        pipeline.onConnected(std::move(name), endpoint);
    }

    static void handleFrame(
        GameServerPipeline& pipeline,
        const std::string& name,
        std::string payload) {
        pipeline.handleFrame(name, std::move(payload));
    }

    static gamenet::DispatchResult submit(
        GameServerPipeline& pipeline,
        const std::string& name,
        std::string payload) {
        return pipeline.submitCommand(
            pipeline.connections_.at(name), std::move(payload));
    }

    static gamenet::DispatchResult send(
        GameServerPipeline& pipeline,
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        std::string payload) {
        return pipeline.sendFrame(endpoint, std::move(payload));
    }

    static std::size_t connectionCount(const GameServerPipeline& pipeline) {
        return pipeline.connections_.size();
    }
};

}  // namespace gamenet::examples

namespace {

class SaturationEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    SaturationEndpoint(
        std::uint64_t id,
        gamenet::net::EventLoop* loop,
        gamenet::transport::EndpointResult sendResult =
            gamenet::transport::EndpointResult::Accepted)
        : id_{id}, loop_(loop), sendResult_(sendResult) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return loop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view) override {
        return open_.load(std::memory_order_acquire)
            ? sendResult_
            : gamenet::transport::EndpointResult::Closed;
    }
    gamenet::transport::EndpointResult close(
        gamenet::transport::CloseReason reason) override {
        closeReason_.store(reason, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        return gamenet::transport::EndpointResult::Accepted;
    }
    gamenet::DispatchResult requestClose(
        gamenet::transport::CloseReason reason) noexcept override {
        (void)close(reason);
        return gamenet::DispatchResult::Accepted;
    }
    bool isOpen() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }
    gamenet::transport::CloseReason closeReason() const noexcept {
        return closeReason_.load(std::memory_order_acquire);
    }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* loop_;
    gamenet::transport::EndpointResult sendResult_;
    std::atomic<bool> open_{true};
    std::atomic<gamenet::transport::CloseReason> closeReason_{
        gamenet::transport::CloseReason::Normal};
};

void continuationQueueFullCloses() {
    gamenet::net::EventLoop loop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    auto endpoint = std::make_shared<SaturationEndpoint>(1, &loop);
    GAMENET_TEST_ASSERT(
        loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);

    gamenet::protocol::PacketFramer codec;
    std::string bytes;
    for (int index = 0; index < 130; ++index) {
        const auto frame = codec.encode("frame-" + std::to_string(index));
        GAMENET_TEST_ASSERT(frame);
        bytes += *frame;
    }
    std::size_t delivered = 0;
    gamenet::examples::GameServerPipelineM2TestPeer::injectIoBytes(
        pipeline,
        endpoint,
        std::move(bytes),
        [&](std::vector<std::string> frames) { delivered += frames.size(); });
    GAMENET_TEST_ASSERT(delivered == 64);
    GAMENET_TEST_ASSERT(!endpoint->isOpen());
    GAMENET_TEST_ASSERT(
        endpoint->closeReason() == gamenet::transport::CloseReason::Overloaded);
}

void logicQueueFullCloses() {
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.logicOptions = {
             .tickInterval = std::chrono::hours(1),
             .maxCommandsPerTick = 1,
             .queueLimits = {
                 .maxCommands = 1,
                 .maxQueuedBytes = 64,
                 .maxPayloadBytes = 64}}});
    auto endpoint = std::make_shared<SaturationEndpoint>(2, &loop);
    gamenet::examples::GameServerPipelineM2TestPeer::addOnlineConnection(
        pipeline, "logic-full", endpoint, "logic-player");
    GAMENET_TEST_ASSERT(
        gamenet::examples::GameServerPipelineM2TestPeer::submit(
            pipeline, "logic-full", "first") ==
        gamenet::DispatchResult::Accepted);
    GAMENET_TEST_ASSERT(
        gamenet::examples::GameServerPipelineM2TestPeer::submit(
            pipeline, "logic-full", "second") ==
        gamenet::DispatchResult::QueueFull);
    GAMENET_TEST_ASSERT(!endpoint->isOpen());
}

void authenticationDispatchQueueFullCloses() {
    gamenet::net::EventLoop loop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    auto endpoint = std::make_shared<SaturationEndpoint>(3, &loop);
    gamenet::examples::GameServerPipelineM2TestPeer::addConnection(
        pipeline, "auth-full", endpoint);
    GAMENET_TEST_ASSERT(
        loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);

    gamenet::examples::GameServerPipelineM2TestPeer::handleFrame(
        pipeline, "auth-full", "AUTH auth-player");

    GAMENET_TEST_ASSERT(!endpoint->isOpen());
    GAMENET_TEST_ASSERT(
        endpoint->closeReason() == gamenet::transport::CloseReason::Overloaded);
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}

void endpointQueueFullCloses() {
    gamenet::net::EventLoop loop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    auto endpoint = std::make_shared<SaturationEndpoint>(4, &loop);
    GAMENET_TEST_ASSERT(
        loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);

    GAMENET_TEST_ASSERT(
        gamenet::examples::GameServerPipelineM2TestPeer::send(
            pipeline, endpoint, "output") ==
        gamenet::DispatchResult::QueueFull);
    GAMENET_TEST_ASSERT(!endpoint->isOpen());
    GAMENET_TEST_ASSERT(
        endpoint->closeReason() == gamenet::transport::CloseReason::Overloaded);
}

void logicOutputToManagementQueueFullCloses() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop managementLoop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::net::EventLoopThread logicThread({}, "pipeline-saturation-logic");
    auto* logicLoop = logicThread.startLoop();
    {
        gamenet::examples::GameServerPipeline pipeline(
            &managementLoop,
            gamenet::net::InetAddress(0, true),
            {
                .logicOptions = {
                    .tickInterval = 1ms,
                    .maxCommandsPerTick = 1,
                },
                .logicLoop = logicLoop,
            });
        pipeline.start();
        auto endpoint = std::make_shared<SaturationEndpoint>(
            5, &managementLoop);
        gamenet::examples::GameServerPipelineM2TestPeer::addOnlineConnection(
            pipeline, "output-management-full", endpoint, "output-player");
        GAMENET_TEST_ASSERT(
            managementLoop.executor().post([&managementLoop] {
                managementLoop.quit();
            }) == gamenet::net::PostResult::Accepted);
        GAMENET_TEST_ASSERT(
            gamenet::examples::GameServerPipelineM2TestPeer::submit(
                pipeline, "output-management-full", "command") ==
            gamenet::DispatchResult::Accepted);

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (endpoint->isOpen() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(!endpoint->isOpen());
        GAMENET_TEST_ASSERT(
            endpoint->closeReason() ==
            gamenet::transport::CloseReason::Overloaded);

        pipeline.stop();
        managementLoop.loop();
    }
    logicThread.stop();
}

void endpointOverloadCloses() {
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    pipeline.start();
    auto endpoint = std::make_shared<SaturationEndpoint>(
        6, &loop, gamenet::transport::EndpointResult::Overloaded);
    GAMENET_TEST_ASSERT(
        gamenet::examples::GameServerPipelineM2TestPeer::send(
            pipeline, endpoint, "output") ==
        gamenet::DispatchResult::Accepted);
    loop.queueInLoop([&] {
        GAMENET_TEST_ASSERT(!endpoint->isOpen());
        GAMENET_TEST_ASSERT(
            endpoint->closeReason() ==
            gamenet::transport::CloseReason::Overloaded);
        pipeline.stop();
        loop.quit();
    });
    loop.loop();
}

void ioToManagementQueueFullClosesRealTcp() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop({
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.ioThreads = 1});
    pipeline.start();

    std::promise<void> sendNow;
    auto sendFuture = sendNow.get_future().share();
    std::atomic<bool> peerClosed{false};
    std::thread client([&] {
        const auto fd = gamenet::test::connectTestClient(pipeline.listenAddress());
        sendFuture.wait();
        gamenet::protocol::PacketFramer codec;
        const auto auth = codec.encode("AUTH saturated-management");
        GAMENET_TEST_ASSERT(auth);
        const auto writeDeadline = std::chrono::steady_clock::now() + 2s;
        std::size_t written = 0;
        while (written < auth->size() &&
               std::chrono::steady_clock::now() < writeDeadline) {
            const auto count = gamenet::net::sockets::write(
                fd, auth->data() + written, auth->size() - written);
            if (count > 0) written += static_cast<std::size_t>(count);
            else std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(written == auth->size());

        char byte{};
        const auto closeDeadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < closeDeadline) {
            const auto count = gamenet::net::sockets::read(fd, &byte, 1);
            if (count == 0) {
                peerClosed.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
        gamenet::test::closeTestSocket(fd);
    });

    loop.runEvery(1ms, [&] {
        if (gamenet::examples::GameServerPipelineM2TestPeer::connectionCount(
                pipeline) != 1) {
            return;
        }
        GAMENET_TEST_ASSERT(
            loop.executor().post([] {}) == gamenet::net::PostResult::Accepted);
        sendNow.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!peerClosed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(peerClosed.load(std::memory_order_acquire));
        pipeline.stop();
        loop.quit();
    });
    loop.runAfter(5s, [&] {
        sendNow.set_value();
        GAMENET_TEST_FAIL("pipeline management saturation timed out");
    });
    loop.loop();
    client.join();
}

}  // namespace

int main() {
    continuationQueueFullCloses();
    logicQueueFullCloses();
    authenticationDispatchQueueFullCloses();
    endpointQueueFullCloses();
    logicOutputToManagementQueueFullCloses();
    endpointOverloadCloses();
    ioToManagementQueueFullClosesRealTcp();
}
