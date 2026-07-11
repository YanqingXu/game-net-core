#pragma once

#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/core/net/TimerId.h"
#include "gamenet/game_logic/LogicLoop.h"
#include "gamenet/game_session/SessionManager.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamenet::examples {

class GameServerPipelineTestPeer;
class GameServerPipelineShutdownTestPeer;

enum class GameServerPipelineStage {
    Io,
    Management,
    Logic,
    Endpoint,
};

struct GameServerPipelineOptions {
    std::size_t maxPendingAuthFrames{32};
    std::size_t maxPendingAuthBytes{64 * 1024};
    int ioThreads{0};
    gamenet::game_session::DuplicateLoginPolicy duplicateLoginPolicy{
        gamenet::game_session::DuplicateLoginPolicy::ReplaceExisting};
    std::chrono::steady_clock::duration authenticationDelay{};
    // Borrowed when non-null. A distinct logic loop must already be running and
    // accepting before Pipeline construction, and must outlive Pipeline stop
    // and destruction. Stop/destroy Pipeline on the management loop before the
    // caller stops or destroys this EventLoop.
    gamenet::net::EventLoop* logicLoop{nullptr};
    std::function<void(GameServerPipelineStage)> stageObserver;
};

class GameServerPipeline {
public:
    GameServerPipeline(
        gamenet::net::EventLoop* loop,
        const gamenet::net::InetAddress& listenAddress,
        GameServerPipelineOptions options = {});
    ~GameServerPipeline();

    GameServerPipeline(const GameServerPipeline&) = delete;
    GameServerPipeline& operator=(const GameServerPipeline&) = delete;

    // Lifecycle mutation and destruction are management-loop-only. With a
    // distinct borrowed logic loop, stop() synchronously waits for an active
    // logic callback and destroys the internal LogicLoop on that owner.
    void start();
    void stop();
    const gamenet::net::InetAddress& listenAddress() const noexcept;
    std::size_t activeSessionCount() const;

private:
    // The peers are compiled only by the example integration tests. They keep
    // exact batching and lifecycle contracts testable without exporting a
    // production API.
    friend class GameServerPipelineTestPeer;
    friend class GameServerPipelineShutdownTestPeer;

    enum class AuthenticationState {
        Unauthenticated,
        Authenticating,
        Online,
        Closing,
    };

    struct AuthenticationAttempt {
        explicit AuthenticationAttempt(std::string playerIdValue)
            : playerId(std::move(playerIdValue)) {}

        std::string playerId;
    };

    struct ConnectionState {
        explicit ConnectionState(
            std::shared_ptr<gamenet::transport::TransportEndpoint> endpointValue)
            : endpoint(std::move(endpointValue)) {}

        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
        gamenet::game_session::SessionId sessionId{};
        AuthenticationState authentication{AuthenticationState::Unauthenticated};
        std::deque<std::string> pendingAuthFrames;
        std::size_t pendingAuthBytes{};
        std::optional<gamenet::net::TimerId> authenticationTimer;
        std::shared_ptr<AuthenticationAttempt> authenticationAttempt;
    };

    struct IoConnectionState;
    struct CallbackState;

    static void handleIoFramerResult(
        const std::shared_ptr<IoConnectionState>& state,
        gamenet::protocol::FrameResult result);
    void injectIoBytesForTesting(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        std::string bytes,
        std::function<void(std::vector<std::string>)> deliver);
    void onConnected(
        std::string connectionName,
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint);
    void onDisconnected(
        const std::string& connectionName,
        gamenet::transport::TransportSessionId transportId);
    void onFrames(
        const std::string& connectionName,
        gamenet::transport::TransportSessionId transportId,
        std::vector<std::string> frames);
    void handleFrame(const std::string& connectionName, std::string payload);
    void completeAuthentication(
        const std::string& connectionName,
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        gamenet::game_session::AuthenticateResult result);
    void beginAuthentication(
        const std::string& connectionName,
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        std::string playerId);
    bool submitCommand(ConnectionState& state, std::string payload);
    void handleLogicOutput(gamenet::game_logic::GameCommand command);
    void runOnLogicLoopAndWait(
        std::function<void()> callback,
        std::atomic<bool>* waitActive = nullptr);
    void closeConnection(ConnectionState& state, gamenet::transport::CloseReason reason);
    void cancelAuthenticationTimer(ConnectionState& state);
    void sendFrame(
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
        std::string payload);

    gamenet::net::EventLoop* loop_;
    GameServerPipelineOptions options_;
    std::shared_ptr<CallbackState> callbackState_;
    gamenet::net::TcpServer server_;
    gamenet::game_session::SessionManager sessions_;
    gamenet::net::EventLoop* logicLoop_;
    gamenet::net::EventLoopExecutor logicExecutor_;
    std::unique_ptr<gamenet::game_logic::LogicLoop> logic_;
    gamenet::protocol::PacketFramer encoder_;
    std::unordered_map<std::string, ConnectionState> connections_;
    std::shared_ptr<std::atomic<std::uint64_t>> nextTransportId_;
    // Published only after distinct-owner stop work has been admitted and
    // cleared after its completion; the shutdown friend seam observes it.
    std::atomic<bool> logicStopWaitActive_{false};
    bool started_{false};
    bool stopped_{false};
};

}  // namespace gamenet::examples
