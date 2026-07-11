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
#include <vector>

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline pipeline(
        &loop, gamenet::net::InetAddress(0, true));
    pipeline.start();

    gamenet::protocol::PacketFramer encoder;
    gamenet::protocol::PacketFramer decoder;
    std::unique_ptr<gamenet::net::TcpClient> client;
    std::vector<std::string> responses;

    client = std::make_unique<gamenet::net::TcpClient>(&loop, pipeline.listenAddress(), "pipeline-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            auto auth = encoder.encode("AUTH player-1");
            GAMENET_TEST_ASSERT(auth.has_value());
            connection->send(std::string_view(*auth).substr(0, 2));
            loop.runAfter(1ms, [connection, auth = std::move(*auth)] {
                connection->send(std::string_view(auth).substr(2));
            });
        }
    });
    client->setMessageCallback([&](const auto& connection, auto* buffer) {
        auto parsed = decoder.push(buffer->retrieveAllAsString());
        for (auto& response : parsed.frames) {
            responses.push_back(response);
            if (response == "AUTH_OK") {
                auto ping = encoder.encode("ping");
                auto pong = encoder.encode("pong");
                GAMENET_TEST_ASSERT(ping.has_value() && pong.has_value());
                connection->send(*ping + *pong);
            }
        }
        if (responses.size() == 3) {
            connection->shutdown();
            client->stop();
        }
    });
    client->connect();

    loop.runEvery(1ms, [&] {
        if (responses.size() == 3 && pipeline.activeSessionCount() == 0) {
            pipeline.stop();
            loop.quit();
        }
    });
    loop.runAfter(5s, [&] { GAMENET_TEST_FAIL("game pipeline integration timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT((responses == std::vector<std::string>{"AUTH_OK", "RESP ping", "RESP pong"}));
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}
