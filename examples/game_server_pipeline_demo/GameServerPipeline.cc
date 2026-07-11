#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace gamenet::examples {

GameServerPipeline::GameServerPipeline(
    gamenet::net::EventLoop* loop,
    const gamenet::net::InetAddress& listenAddress)
    : loop_(loop),
      server_(loop, listenAddress, "game_server_pipeline_demo"),
      sessions_(loop),
      logic_(loop, {.tickInterval = std::chrono::milliseconds(5), .maxCommandsPerTick = 64}) {
    if (!loop_) {
        throw std::invalid_argument("GameServerPipeline requires an owner loop");
    }
    server_.setConnectionCallback([this](const auto& connection) { onConnection(connection); });
    server_.setMessageCallback(
        [this](const auto& connection, auto* buffer) { onMessage(connection, buffer); });

    logic_.setHandler([](gamenet::game_logic::GameCommand command)
                          -> std::optional<gamenet::game_logic::GameCommand> {
        command.payload = "RESP " + command.payload;
        return command;
    });
    logic_.setOutputCallback([this](gamenet::game_logic::GameCommand command) {
        auto session = sessions_.findByTransport(command.transportId);
        if (!session || session->sessionId() != command.sessionId) {
            return;
        }
        sendFrame(session->endpoint(), std::move(command.payload));
    });
}

GameServerPipeline::~GameServerPipeline() {
    if (started_) {
        stop();
    }
}

void GameServerPipeline::start() {
    loop_->assertInLoopThread();
    if (started_) return;
    started_ = true;
    logic_.start();
    server_.start();
}

void GameServerPipeline::stop() {
    loop_->assertInLoopThread();
    if (!started_) return;
    started_ = false;
    logic_.stop();
    server_.stop();
}

const gamenet::net::InetAddress& GameServerPipeline::listenAddress() const noexcept {
    return server_.listenAddress();
}

std::size_t GameServerPipeline::activeSessionCount() const { return sessions_.size(); }

void GameServerPipeline::onConnection(const gamenet::net::TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    if (connection->connected()) {
        auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
            gamenet::transport::TransportSessionId{nextTransportId_++}, connection);
        connections_.try_emplace(connection->name(), std::move(endpoint));
        return;
    }

    const auto found = connections_.find(connection->name());
    if (found == connections_.end()) return;
    sessions_.postOffline(found->second.endpoint->id());
    connections_.erase(found);
}

void GameServerPipeline::onMessage(
    const gamenet::net::TcpConnectionPtr& connection,
    gamenet::net::Buffer* buffer) {
    loop_->assertInLoopThread();
    const auto found = connections_.find(connection->name());
    if (found == connections_.end()) return;

    auto result = found->second.framer.push(buffer->retrieveAllAsString());
    if (result.status == gamenet::protocol::FrameStatus::FrameTooLarge ||
        result.status == gamenet::protocol::FrameStatus::Faulted) {
        found->second.endpoint->close(gamenet::transport::CloseReason::ProtocolError);
        return;
    }
    for (auto& frame : result.frames) {
        handleFrame(connection->name(), std::move(frame));
    }
}

void GameServerPipeline::handleFrame(const std::string& connectionName, std::string payload) {
    auto found = connections_.find(connectionName);
    if (found == connections_.end()) return;
    auto& state = found->second;

    if (state.sessionId == 0) {
        constexpr std::string_view prefix = "AUTH ";
        if (!payload.starts_with(prefix) || payload.size() == prefix.size()) {
            state.endpoint->close(gamenet::transport::CloseReason::ProtocolError);
            return;
        }
        auto endpoint = state.endpoint;
        sessions_.postAuthenticate(
            payload.substr(prefix.size()),
            endpoint,
            [this, connectionName, endpoint](gamenet::game_session::AuthenticateResult result) {
                const auto current = connections_.find(connectionName);
                if (current == connections_.end() || current->second.endpoint->id() != endpoint->id()) {
                    return;
                }
                if (result.status == gamenet::game_session::AuthenticateStatus::Rejected || !result.session) {
                    endpoint->ownerLoop()->queueInLoop([endpoint] {
                        endpoint->close(gamenet::transport::CloseReason::Replaced);
                    });
                    return;
                }
                current->second.sessionId = result.session->sessionId();
                sendFrame(endpoint, "AUTH_OK");
            });
        return;
    }

    gamenet::game_logic::GameCommand command;
    command.sessionId = state.sessionId;
    command.transportId = state.endpoint->id();
    command.payload = std::move(payload);
    if (logic_.submit(std::move(command)) != gamenet::game_logic::SubmitResult::Accepted) {
        state.endpoint->close(gamenet::transport::CloseReason::Overloaded);
    }
}

void GameServerPipeline::sendFrame(
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
    std::string payload) {
    auto frame = encoder_.encode(payload);
    if (!frame) {
        endpoint->ownerLoop()->queueInLoop([endpoint] {
            endpoint->close(gamenet::transport::CloseReason::ProtocolError);
        });
        return;
    }
    endpoint->ownerLoop()->queueInLoop(
        [endpoint, frame = std::move(*frame)] { endpoint->send(frame); });
}

}  // namespace gamenet::examples
