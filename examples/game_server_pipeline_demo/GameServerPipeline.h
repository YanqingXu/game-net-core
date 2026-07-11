#pragma once

#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/game_logic/LogicLoop.h"
#include "gamenet/game_session/SessionManager.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace gamenet::examples {

class GameServerPipeline {
public:
    GameServerPipeline(gamenet::net::EventLoop* loop, const gamenet::net::InetAddress& listenAddress);
    ~GameServerPipeline();

    GameServerPipeline(const GameServerPipeline&) = delete;
    GameServerPipeline& operator=(const GameServerPipeline&) = delete;

    void start();
    void stop();
    const gamenet::net::InetAddress& listenAddress() const noexcept;
    std::size_t activeSessionCount() const;

private:
    struct ConnectionState {
        explicit ConnectionState(
            std::shared_ptr<gamenet::transport::TransportEndpoint> endpointValue)
            : endpoint(std::move(endpointValue)) {}

        gamenet::protocol::PacketFramer framer;
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
        gamenet::game_session::SessionId sessionId{};
    };

    void onConnection(const gamenet::net::TcpConnectionPtr& connection);
    void onMessage(const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* buffer);
    void handleFrame(const std::string& connectionName, std::string payload);
    void sendFrame(
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        std::string payload);

    gamenet::net::EventLoop* loop_;
    gamenet::net::TcpServer server_;
    gamenet::game_session::SessionManager sessions_;
    gamenet::game_logic::LogicLoop logic_;
    gamenet::protocol::PacketFramer encoder_;
    std::unordered_map<std::string, ConnectionState> connections_;
    std::uint64_t nextTransportId_{1};
    bool started_{false};
};

}  // namespace gamenet::examples
