#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <chrono>
#include <array>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gamenet::examples {

class GameServerPipelineTestPeer {
public:
    static void injectFrameBatch(
        GameServerPipeline& pipeline,
        std::string connectionName,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        std::vector<std::string> frames) {
        const auto transportId = endpoint->id();
        pipeline.onConnected(connectionName, std::move(endpoint));
        pipeline.onFrames(connectionName, transportId, std::move(frames));
    }

    static void injectIoBytes(
        GameServerPipeline& pipeline,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        std::string bytes,
        std::function<void(std::vector<std::string>)> deliver) {
        pipeline.injectIoBytesForTesting(
            std::move(endpoint), std::move(bytes), std::move(deliver));
    }
};

}  // namespace gamenet::examples

namespace {

class BatchEndpoint final : public gamenet::transport::TransportEndpoint {
public:
    BatchEndpoint(std::uint64_t id, gamenet::net::EventLoop* ownerLoop)
        : id_{id}, ownerLoop_(ownerLoop) {}

    gamenet::transport::TransportSessionId id() const noexcept override { return id_; }
    gamenet::net::EventLoopExecutor ownerExecutor() const noexcept override {
        return ownerLoop_->executor();
    }
    gamenet::transport::EndpointResult send(std::string_view bytes) override {
        GAMENET_TEST_ASSERT(ownerLoop_->isInLoopThread());
        auto parsed = decoder_.push(bytes);
        GAMENET_TEST_ASSERT(parsed.status == gamenet::protocol::FrameStatus::FramesReady);
        responses_.insert(responses_.end(), parsed.frames.begin(), parsed.frames.end());
        return open_ ? gamenet::transport::EndpointResult::Accepted
                     : gamenet::transport::EndpointResult::Closed;
    }
    gamenet::transport::EndpointResult close(gamenet::transport::CloseReason) override {
        GAMENET_TEST_ASSERT(ownerLoop_->isInLoopThread());
        open_ = false;
        return gamenet::transport::EndpointResult::Accepted;
    }
    bool isOpen() const noexcept override { return open_; }
    const std::vector<std::string>& responses() const noexcept { return responses_; }

private:
    gamenet::transport::TransportSessionId id_;
    gamenet::net::EventLoop* ownerLoop_;
    gamenet::protocol::PacketFramer decoder_;
    std::vector<std::string> responses_;
    bool open_{true};
};

void verifyDeferredIoContinuation() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    std::string bytes;
    std::vector<std::string> expected;
    for (std::size_t index = 0; index < 130; ++index) {
        expected.push_back("continuation-" + std::to_string(index));
        const auto frame = codec.encode(expected.back());
        GAMENET_TEST_ASSERT(frame);
        bytes += *frame;
    }

    auto endpoint = std::make_shared<BatchEndpoint>(9000, &loop);
    std::vector<std::size_t> batchSizes;
    std::vector<std::string> delivered;
    gamenet::examples::GameServerPipelineTestPeer::injectIoBytes(
        pipeline,
        endpoint,
        std::move(bytes),
        [&](std::vector<std::string> frames) {
            batchSizes.push_back(frames.size());
            delivered.insert(
                delivered.end(),
                std::make_move_iterator(frames.begin()),
                std::make_move_iterator(frames.end()));
        });

    GAMENET_TEST_ASSERT((batchSizes == std::vector<std::size_t>{64}));
    GAMENET_TEST_ASSERT(delivered.size() == 64);
    loop.runEvery(1ms, [&] {
        if (delivered.size() != expected.size()) return;
        GAMENET_TEST_ASSERT((batchSizes == std::vector<std::size_t>{64, 64, 2}));
        GAMENET_TEST_ASSERT(delivered == expected);
        pipeline.stop();
        loop.quit();
    });
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("deferred I/O continuation timed out"); });
    loop.loop();
}

void verifyExactSameBatchAuthentication() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    pipeline.start();

    auto endpoint = std::make_shared<BatchEndpoint>(9001, &loop);
    gamenet::examples::GameServerPipelineTestPeer::injectFrameBatch(
        pipeline,
        "deterministic-same-batch",
        endpoint,
        {"AUTH exact-batch-player", "early-command"});

    loop.runEvery(1ms, [&] {
        if (endpoint->responses().size() != 2) return;
        GAMENET_TEST_ASSERT(
            (endpoint->responses() ==
             std::vector<std::string>{"AUTH_OK", "RESP early-command"}));
        pipeline.stop();
        loop.quit();
    });
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("exact same-batch authentication timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT(
        (endpoint->responses() ==
         std::vector<std::string>{"AUTH_OK", "RESP early-command"}));
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    verifyDeferredIoContinuation();
    verifyExactSameBatchAuthentication();

    gamenet::net::EventLoop loop;
    gamenet::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();
    const auto managementThread = std::this_thread::get_id();
    std::mutex stagesMutex;
    std::array<std::thread::id, 4> stageThreads{};
    auto observeStage = [&](gamenet::examples::GameServerPipelineStage stage) {
        std::lock_guard lock(stagesMutex);
        auto& observed = stageThreads[static_cast<std::size_t>(stage)];
        if (observed == std::thread::id{}) {
            observed = std::this_thread::get_id();
        } else {
            GAMENET_TEST_ASSERT(observed == std::this_thread::get_id());
        }
    };
    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.maxPendingAuthFrames = 4,
         .maxPendingAuthBytes = 64,
         .ioThreads = 1,
         .duplicateLoginPolicy = gamenet::game_session::DuplicateLoginPolicy::RejectNew,
         .logicLoop = logicLoop,
         .stageObserver = observeStage});
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    gamenet::protocol::PacketFramer decoder;
    gamenet::protocol::PacketFramer rejectedDecoder;
    std::vector<std::string> responses;
    std::vector<std::string> rejectedResponses;
    std::weak_ptr<gamenet::net::TcpConnection> firstConnection;
    std::unique_ptr<gamenet::net::TcpClient> rejectedClient;
    bool rejectedConnected = false;
    bool rejectedDisconnected = false;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), "pipeline-auth-state-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        firstConnection = connection;
        const auto auth = codec.encode("AUTH player-auth-state");
        const auto early = codec.encode("early-command");
        GAMENET_TEST_ASSERT(auth && early);
        connection->send(*auth + *early);
    });
    client->setMessageCallback([&](const auto&, auto* buffer) {
        auto parsed = decoder.push(buffer->retrieveAllAsString());
        responses.insert(responses.end(), parsed.frames.begin(), parsed.frames.end());
        if (responses.size() == 2) {
            GAMENET_TEST_ASSERT(responses[0] == "AUTH_OK");
            GAMENET_TEST_ASSERT(responses[1] == "RESP early-command");
            GAMENET_TEST_ASSERT(!rejectedClient);
            rejectedClient = std::make_unique<gamenet::net::TcpClient>(
                &loop, pipeline.listenAddress(), "pipeline-auth-rejected-client");
            rejectedClient->setConnectionCallback(
                [&](const gamenet::net::TcpConnectionPtr& rejectedConnection) {
                    if (rejectedConnection->connected()) {
                        rejectedConnected = true;
                        const auto duplicateAuth = codec.encode("AUTH player-auth-state");
                        const auto pending = codec.encode("must-be-discarded");
                        GAMENET_TEST_ASSERT(duplicateAuth && pending);
                        rejectedConnection->send(*duplicateAuth + *pending);
                        return;
                    }

                    rejectedDisconnected = true;
                    GAMENET_TEST_ASSERT(rejectedResponses.empty());
                    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 1);
                    if (const auto first = firstConnection.lock()) first->forceClose();
                    pipeline.stop();
                    loop.quit();
                });
            rejectedClient->setMessageCallback([&](const auto&, auto* rejectedBuffer) {
                auto parsed = rejectedDecoder.push(rejectedBuffer->retrieveAllAsString());
                rejectedResponses.insert(
                    rejectedResponses.end(), parsed.frames.begin(), parsed.frames.end());
                GAMENET_TEST_ASSERT(rejectedResponses.empty());
            });
            rejectedClient->connect();
        }
    });
    client->connect();
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("same-batch AUTH/command timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT((responses == std::vector<std::string>{"AUTH_OK", "RESP early-command"}));
    GAMENET_TEST_ASSERT(rejectedConnected && rejectedDisconnected);
    GAMENET_TEST_ASSERT(rejectedResponses.empty());
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
    {
        std::lock_guard lock(stagesMutex);
        const auto io = stageThreads[static_cast<std::size_t>(
            gamenet::examples::GameServerPipelineStage::Io)];
        const auto management = stageThreads[static_cast<std::size_t>(
            gamenet::examples::GameServerPipelineStage::Management)];
        const auto logic = stageThreads[static_cast<std::size_t>(
            gamenet::examples::GameServerPipelineStage::Logic)];
        const auto endpoint = stageThreads[static_cast<std::size_t>(
            gamenet::examples::GameServerPipelineStage::Endpoint)];
        GAMENET_TEST_ASSERT(io != std::thread::id{});
        GAMENET_TEST_ASSERT(management == managementThread);
        GAMENET_TEST_ASSERT(logic != std::thread::id{});
        GAMENET_TEST_ASSERT(endpoint == io);
        GAMENET_TEST_ASSERT(io != management);
        GAMENET_TEST_ASSERT(logic != management);
        GAMENET_TEST_ASSERT(logic != io);
    }
}
