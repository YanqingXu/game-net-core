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
#include <optional>
#include <string>

int main() {
    using namespace std::chrono_literals;
    constexpr auto idleTimeout = 70ms;
    constexpr std::size_t heartbeatCount = 7;

    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {
            .sessionIdleTimeout = idleTimeout,
            .sessionSweepInterval = 5ms,
        });
    pipeline.start();

    gamenet::protocol::PacketFramer encoder;
    gamenet::protocol::PacketFramer decoder;
    std::unique_ptr<gamenet::net::TcpClient> client;
    std::weak_ptr<gamenet::net::TcpConnection> connection;
    std::optional<gamenet::net::TimerId> heartbeatTimer;
    std::optional<std::chrono::steady_clock::time_point> authenticatedAt;
    std::optional<std::chrono::steady_clock::time_point> heartbeatStoppedAt;
    std::optional<std::chrono::steady_clock::time_point> disconnectedAt;
    std::size_t heartbeatsSent = 0;
    std::size_t heartbeatResponses = 0;
    bool disconnected = false;

    client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), "pipeline-idle-heartbeat-client");
    client->setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& current) {
            if (current->connected()) {
                connection = current;
                const auto auth = encoder.encode("AUTH idle-heartbeat-player");
                GAMENET_TEST_ASSERT(auth);
                current->send(*auth);
                return;
            }
            disconnected = true;
            disconnectedAt = std::chrono::steady_clock::now();
            client->stop();
        });
    client->setMessageCallback([&](const auto&, auto* buffer) {
        auto parsed = decoder.push(buffer->retrieveAllAsString());
        for (const auto& response : parsed.frames) {
            if (response == "AUTH_OK") {
                authenticatedAt = std::chrono::steady_clock::now();
                heartbeatTimer = loop.runEvery(20ms, [&] {
                    const auto current = connection.lock();
                    GAMENET_TEST_ASSERT(current && current->connected());
                    const auto heartbeat =
                        encoder.encode("heartbeat-" + std::to_string(heartbeatsSent));
                    GAMENET_TEST_ASSERT(heartbeat);
                    current->send(*heartbeat);
                    ++heartbeatsSent;
                    if (heartbeatsSent == heartbeatCount) {
                        loop.cancel(*heartbeatTimer);
                        heartbeatTimer.reset();
                        heartbeatStoppedAt = std::chrono::steady_clock::now();
                    }
                });
            } else if (response.starts_with("RESP heartbeat-")) {
                ++heartbeatResponses;
            }
        }
    });
    client->connect();

    loop.runEvery(5ms, [&] {
        if (!disconnected || pipeline.activeSessionCount() != 0) return;
        pipeline.stop();
        loop.quit();
    });
    loop.runAfter(3s, [&] {
        pipeline.stop();
        GAMENET_TEST_FAIL("pipeline heartbeat/idle integration timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(authenticatedAt.has_value());
    GAMENET_TEST_ASSERT(heartbeatStoppedAt.has_value());
    GAMENET_TEST_ASSERT(disconnectedAt.has_value());
    GAMENET_TEST_ASSERT(heartbeatsSent == heartbeatCount);
    GAMENET_TEST_ASSERT(heartbeatResponses == heartbeatCount);
    GAMENET_TEST_ASSERT(*heartbeatStoppedAt - *authenticatedAt > idleTimeout);
    GAMENET_TEST_ASSERT(*disconnectedAt - *heartbeatStoppedAt >= idleTimeout - 10ms);
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}
