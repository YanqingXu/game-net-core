#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

void runRejectedScenario(
    std::string_view name,
    const std::vector<std::string>& payloads,
    gamenet::examples::GameServerPipelineOptions options,
    bool clientInitiatesDisconnect = false) {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true), options);
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    std::string bytes;
    for (const auto& payload : payloads) {
        const auto frame = codec.encode(payload);
        GAMENET_TEST_ASSERT(frame);
        bytes += *frame;
    }

    bool connectedOnce = false;
    std::size_t responseBytes = 0;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), std::string(name));
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            connectedOnce = true;
            connection->send(bytes);
            if (clientInitiatesDisconnect) {
                connection->shutdown();
                loop.runAfter(10ms, [&] { client->stop(); });
                loop.runAfter(250ms, [&] {
                    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
                    GAMENET_TEST_ASSERT(responseBytes == 0);
                    pipeline.stop();
                    loop.quit();
                });
            }
            return;
        }
        if (!connectedOnce || clientInitiatesDisconnect) return;
        loop.runAfter(20ms, [&] {
            GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
            GAMENET_TEST_ASSERT(responseBytes == 0);
            pipeline.stop();
            loop.quit();
        });
    });
    client->setMessageCallback([&](const auto&, auto* buffer) {
        responseBytes += buffer->readableBytes();
        buffer->retrieveAll();
    });
    client->connect();
    loop.runAfter(3s, [&] { GAMENET_TEST_FAIL("pipeline handoff rejection timed out"); });
    loop.loop();
}

}  // namespace

int main() {
    runRejectedScenario(
        "disconnect-before-auth",
        {"AUTH disconnect-player"},
        {.maxPendingAuthFrames = 4,
         .maxPendingAuthBytes = 64,
         .ioThreads = 1,
         .authenticationDelay = std::chrono::milliseconds(100)},
        true);
    runRejectedScenario(
        "duplicate-auth",
        {"AUTH duplicate-player", "AUTH duplicate-player"},
        {.maxPendingAuthFrames = 4, .maxPendingAuthBytes = 64, .ioThreads = 1});
    runRejectedScenario(
        "pending-auth-frame-overflow",
        {"AUTH overflow-player", "one", "two"},
        {.maxPendingAuthFrames = 1, .maxPendingAuthBytes = 64, .ioThreads = 1});
    runRejectedScenario(
        "pending-auth-byte-overflow",
        {"AUTH byte-overflow-player", "four"},
        {.maxPendingAuthFrames = 4, .maxPendingAuthBytes = 3, .ioThreads = 1});
}
