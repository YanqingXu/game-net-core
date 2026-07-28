#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop managementLoop;
    gamenet::net::EventLoopThread logicThread;
    auto* logicOwner = logicThread.startLoop();
    std::atomic<int> logicHandlerCalls{0};

    gamenet::examples::GameServerPipeline pipeline(
        &managementLoop,
        gamenet::net::InetAddress(0, true),
        {.ioThreads = 1,
         .logicLoop = logicOwner,
         .stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
             if (stage == gamenet::examples::GameServerPipelineStage::Logic) {
                 logicHandlerCalls.fetch_add(1, std::memory_order_relaxed);
             }
         }});
    pipeline.start();

    gamenet::protocol::PacketFramer encoder;
    gamenet::protocol::PacketFramer firstDecoder;
    gamenet::protocol::PacketFramer secondDecoder;
    std::shared_ptr<gamenet::net::TcpConnection> firstConnection;
    std::shared_ptr<gamenet::net::TcpConnection> secondConnection;
    std::vector<std::string> firstResponses;
    std::vector<std::string> secondResponses;
    std::promise<void> releaseLogic;
    auto releaseLogicFuture = releaseLogic.get_future().share();
    std::atomic<bool> logicBlocked{false};
    bool staleSubmitted = false;
    bool replacementStarted = false;
    bool released = false;
    bool firstDisconnected = false;

    auto first = std::make_unique<gamenet::net::TcpClient>(
        &managementLoop, pipeline.listenAddress(), "generation-first");
    auto second = std::make_unique<gamenet::net::TcpClient>(
        &managementLoop, pipeline.listenAddress(), "generation-second");

    first->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            firstConnection = connection;
            const auto auth = encoder.encode("AUTH same-player");
            GAMENET_TEST_ASSERT(auth);
            connection->send(*auth);
        } else {
            firstDisconnected = true;
        }
    });
    first->setMessageCallback([&](const auto&, auto* buffer) {
        const auto parsed = firstDecoder.push(buffer->retrieveAllAsString());
        firstResponses.insert(
            firstResponses.end(), parsed.frames.begin(), parsed.frames.end());
        if (!firstResponses.empty() && firstResponses.front() == "AUTH_OK" &&
            !logicBlocked.load(std::memory_order_acquire)) {
            logicOwner->queueInLoop([&] {
                logicBlocked.store(true, std::memory_order_release);
                releaseLogicFuture.wait();
            });
        }
    });

    second->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        secondConnection = connection;
        const auto auth = encoder.encode("AUTH same-player");
        GAMENET_TEST_ASSERT(auth);
        connection->send(*auth);
    });
    second->setMessageCallback([&](const auto&, auto* buffer) {
        const auto parsed = secondDecoder.push(buffer->retrieveAllAsString());
        for (const auto& frame : parsed.frames) {
            secondResponses.push_back(frame);
            if (frame == "AUTH_OK" && !released) {
                released = true;
                releaseLogic.set_value();
                const auto fresh = encoder.encode("fresh-command");
                GAMENET_TEST_ASSERT(fresh);
                secondConnection->send(*fresh);
            }
        }
    });

    first->connect();
    managementLoop.runEvery(1ms, [&] {
        if (logicBlocked.load(std::memory_order_acquire) &&
            firstConnection && !staleSubmitted) {
            staleSubmitted = true;
            const auto stale = encoder.encode("stale-command");
            GAMENET_TEST_ASSERT(stale);
            firstConnection->send(*stale);
            managementLoop.runAfter(20ms, [&] {
                replacementStarted = true;
                second->connect();
            });
        }
        if (released && firstDisconnected &&
            secondResponses.size() == 2) {
            GAMENET_TEST_ASSERT(
                secondResponses[0] == "AUTH_OK" &&
                secondResponses[1] == "RESP fresh-command");
            GAMENET_TEST_ASSERT(firstResponses.size() == 1);
            GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 1);
            GAMENET_TEST_ASSERT(
                logicHandlerCalls.load(std::memory_order_relaxed) == 1);
            first->stop();
            secondConnection->shutdown();
            second->stop();
            pipeline.stop();
            managementLoop.quit();
        }
    });
    managementLoop.runAfter(5s, [&] {
        if (!released) releaseLogic.set_value();
        GAMENET_TEST_FAIL("pipeline binding-generation integration timed out");
    });
    managementLoop.loop();

    GAMENET_TEST_ASSERT(staleSubmitted);
    GAMENET_TEST_ASSERT(replacementStarted);
    logicThread.stop();
}
