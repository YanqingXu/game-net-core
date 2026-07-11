#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include <any>
#include <future>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gamenet::examples {
namespace {

gamenet::net::EventLoop* requireManagementLoop(gamenet::net::EventLoop* loop) {
    if (!loop) throw std::invalid_argument("GameServerPipeline requires a management loop");
    return loop;
}

}  // namespace

struct GameServerPipeline::CallbackState {
    bool active() const noexcept { return alive.load(std::memory_order_acquire); }
    void revoke() noexcept { alive.store(false, std::memory_order_release); }

    struct LogicCallbackScope {
        explicit LogicCallbackScope(std::shared_ptr<CallbackState> stateValue)
            : state(std::move(stateValue)) {
            state->logicCallbackDepth.fetch_add(1, std::memory_order_acq_rel);
        }
        ~LogicCallbackScope() {
            state->logicCallbackDepth.fetch_sub(1, std::memory_order_acq_rel);
        }

        std::shared_ptr<CallbackState> state;
    };

    std::atomic<bool> alive{true};
    std::atomic<std::size_t> logicCallbackDepth{0};
};

struct GameServerPipeline::IoConnectionState {
    std::string connectionName;
    gamenet::protocol::PacketFramer framer;
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
    gamenet::net::EventLoopExecutor ownerExecutor;
    std::shared_ptr<CallbackState> callbackState;
    std::function<void(
        std::string,
        gamenet::transport::TransportSessionId,
        std::vector<std::string>)>
        deliverFrames;
    bool closing{false};
    bool continuationQueued{false};
};

GameServerPipeline::GameServerPipeline(
    gamenet::net::EventLoop* loop,
    const gamenet::net::InetAddress& listenAddress,
    GameServerPipelineOptions options)
    : loop_(requireManagementLoop(loop)),
      options_(std::move(options)),
      callbackState_(std::make_shared<CallbackState>()),
      server_(loop_, listenAddress, "game_server_pipeline_demo"),
      sessions_(
          loop_,
          gamenet::game_session::SessionManager::Options{
              .duplicateLogin = options_.duplicateLoginPolicy,
          }),
      logicLoop_(options_.logicLoop ? options_.logicLoop : loop_),
      logicExecutor_(logicLoop_->executor()),
      nextTransportId_(std::make_shared<std::atomic<std::uint64_t>>(1)) {
    if (options_.ioThreads < 0 ||
        options_.authenticationDelay < std::chrono::steady_clock::duration::zero() ||
        !logicExecutor_.available()) {
        throw std::invalid_argument(
            "GameServerPipeline requires non-negative IO threads/auth delay and a live logic loop");
    }

    server_.setThreadNum(options_.ioThreads);
    const auto callbackState = callbackState_;
    const auto managementExecutor = loop_->executor();
    const auto stageObserver = options_.stageObserver;
    const auto transportIds = nextTransportId_;

    server_.setConnectionCallback(
        [this, callbackState, managementExecutor, transportIds](
            const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                if (!callbackState->active()) {
                    connection->forceClose();
                    return;
                }
                auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
                    gamenet::transport::TransportSessionId{
                        transportIds->fetch_add(1, std::memory_order_relaxed)},
                    connection);
                auto ioState = std::make_shared<IoConnectionState>();
                ioState->connectionName = connection->name();
                ioState->endpoint = endpoint;
                ioState->ownerExecutor = endpoint->ownerExecutor();
                ioState->callbackState = callbackState;
                ioState->deliverFrames =
                    [this, callbackState, managementExecutor](
                        std::string connectionName,
                        gamenet::transport::TransportSessionId transportId,
                        std::vector<std::string> frames) mutable {
                        if (!callbackState->active()) return;
                        (void)managementExecutor.tryQueue(
                            [this,
                             callbackState,
                             connectionName = std::move(connectionName),
                             transportId,
                             frames = std::move(frames)]() mutable {
                                if (callbackState->active()) {
                                    onFrames(connectionName, transportId, std::move(frames));
                                }
                            });
                    };
                if (!callbackState->active()) {
                    endpoint->close(gamenet::transport::CloseReason::GoingAway);
                    return;
                }
                connection->setContext(ioState);

                auto connectionName = ioState->connectionName;
                auto admit = [this,
                              callbackState,
                              connectionName = std::move(connectionName),
                              endpoint = std::move(endpoint)]() mutable {
                    if (callbackState->active()) {
                        onConnected(std::move(connectionName), std::move(endpoint));
                    }
                };
                if (managementExecutor.isInOwnerThread()) {
                    admit();
                } else {
                    (void)managementExecutor.tryQueue(std::move(admit));
                }
                return;
            }

            const auto* context =
                std::any_cast<std::shared_ptr<IoConnectionState>>(&connection->getContext());
            if (!context || !*context) return;
            auto ioState = *context;
            ioState->closing = true;
            connection->setContext(std::any{});
            auto disconnect = [this,
                               callbackState,
                               connectionName = ioState->connectionName,
                               transportId = ioState->endpoint->id()] {
                if (callbackState->active()) onDisconnected(connectionName, transportId);
            };
            if (managementExecutor.isInOwnerThread()) {
                disconnect();
            } else {
                (void)managementExecutor.tryQueue(std::move(disconnect));
            }
        });

    server_.setMessageCallback(
        [callbackState, stageObserver](const auto& connection, auto* buffer) {
            if (!callbackState->active()) {
                buffer->retrieveAll();
                return;
            }
            if (stageObserver) stageObserver(GameServerPipelineStage::Io);
            if (!callbackState->active()) {
                buffer->retrieveAll();
                return;
            }
            const auto* context =
                std::any_cast<std::shared_ptr<IoConnectionState>>(&connection->getContext());
            if (!context || !*context || (*context)->closing) {
                buffer->retrieveAll();
                return;
            }
            auto ioState = *context;
            handleIoFramerResult(
                ioState, ioState->framer.push(buffer->retrieveAllAsString()));
        });

    runOnLogicLoopAndWait([this, callbackState, managementExecutor, stageObserver] {
        auto logic = std::make_unique<gamenet::game_logic::LogicLoop>(
            logicLoop_,
            gamenet::game_logic::LogicLoopOptions{
                .tickInterval = std::chrono::milliseconds(5),
                .maxCommandsPerTick = 64,
            });
        logic->setHandler(
            [callbackState, stageObserver](gamenet::game_logic::GameCommand command)
                -> std::optional<gamenet::game_logic::GameCommand> {
                CallbackState::LogicCallbackScope scope(callbackState);
                if (!callbackState->active()) return std::nullopt;
                if (stageObserver) stageObserver(GameServerPipelineStage::Logic);
                if (!callbackState->active()) return std::nullopt;
                command.payload = "RESP " + command.payload;
                return command;
            });
        logic->setOutputCallback(
            [this, callbackState, managementExecutor](
                gamenet::game_logic::GameCommand command) mutable {
                if (!callbackState->active()) return;
                (void)managementExecutor.tryQueue(
                    [this, callbackState, command = std::move(command)]() mutable {
                        if (callbackState->active()) handleLogicOutput(std::move(command));
                    });
            });
        logic_ = std::move(logic);
    });
}

GameServerPipeline::~GameServerPipeline() {
    if (!stopped_) stop();
    callbackState_->revoke();
}

void GameServerPipeline::start() {
    loop_->assertInLoopThread();
    if (started_) return;
    if (stopped_) throw std::logic_error("GameServerPipeline cannot restart after stop");
    runOnLogicLoopAndWait([this] { logic_->start(); });
    server_.start();
    started_ = true;
}

void GameServerPipeline::stop() {
    loop_->assertInLoopThread();
    if (stopped_) return;
    started_ = false;
    stopped_ = true;
    callbackState_->revoke();

    if (logic_) {
        const auto callbackState = callbackState_;
        runOnLogicLoopAndWait(
            [this, callbackState] {
                (void)logic_->stop();
                if (callbackState->logicCallbackDepth.load(std::memory_order_acquire) == 0) {
                    logic_.reset();
                    return;
                }

                std::shared_ptr<gamenet::game_logic::LogicLoop> retired(logic_.release());
                logicLoop_->queueInLoop([retired = std::move(retired)] { (void)retired; });
            },
            &logicStopWaitActive_);
    }
    server_.stop();
    for (auto& [name, state] : connections_) {
        (void)name;
        cancelAuthenticationTimer(state);
        state.authentication = AuthenticationState::Closing;
        state.pendingAuthFrames.clear();
        state.pendingAuthBytes = 0;
        (void)sessions_.offline(state.endpoint->id());
    }
    connections_.clear();
    sessions_.shutdown();
}

const gamenet::net::InetAddress& GameServerPipeline::listenAddress() const noexcept {
    return server_.listenAddress();
}

std::size_t GameServerPipeline::activeSessionCount() const { return sessions_.size(); }

void GameServerPipeline::handleIoFramerResult(
    const std::shared_ptr<IoConnectionState>& state,
    gamenet::protocol::FrameResult result) {
    if (state->closing || !state->callbackState->active()) {
        state->closing = true;
        return;
    }
    if (result.status == gamenet::protocol::FrameStatus::FrameTooLarge ||
        result.status == gamenet::protocol::FrameStatus::BufferLimitExceeded ||
        result.status == gamenet::protocol::FrameStatus::Faulted) {
        state->closing = true;
        if (state->callbackState->active()) {
            state->endpoint->close(gamenet::transport::CloseReason::ProtocolError);
        }
        return;
    }

    if (!result.frames.empty()) {
        state->deliverFrames(
            state->connectionName, state->endpoint->id(), std::move(result.frames));
    }
    if (!result.needsContinuation || state->continuationQueued) return;

    state->continuationQueued = true;
    if (!state->ownerExecutor.tryQueue([state] {
            state->continuationQueued = false;
            if (!state->callbackState->active()) {
                state->closing = true;
                return;
            }
            if (!state->closing) {
                handleIoFramerResult(state, state->framer.push({}));
            }
        })) {
        state->continuationQueued = false;
        state->closing = true;
    }
}

void GameServerPipeline::injectIoBytesForTesting(
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    std::string bytes,
    std::function<void(std::vector<std::string>)> deliver) {
    auto state = std::make_shared<IoConnectionState>();
    state->connectionName = "deterministic-io-continuation";
    state->endpoint = std::move(endpoint);
    state->ownerExecutor = state->endpoint->ownerExecutor();
    state->callbackState = callbackState_;
    state->deliverFrames =
        [deliver = std::move(deliver)](
            std::string,
            gamenet::transport::TransportSessionId,
            std::vector<std::string> frames) mutable { deliver(std::move(frames)); };
    handleIoFramerResult(state, state->framer.push(bytes));
}

void GameServerPipeline::onConnected(
    std::string connectionName,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint) {
    loop_->assertInLoopThread();
    if (stopped_ || !callbackState_->active()) {
        const auto executor = endpoint->ownerExecutor();
        (void)executor.tryQueue([endpoint = std::move(endpoint)] {
            endpoint->close(gamenet::transport::CloseReason::GoingAway);
        });
        return;
    }
    connections_.try_emplace(std::move(connectionName), std::move(endpoint));
}

void GameServerPipeline::onDisconnected(
    const std::string& connectionName,
    gamenet::transport::TransportSessionId transportId) {
    loop_->assertInLoopThread();
    if (!callbackState_->active()) return;
    const auto found = connections_.find(connectionName);
    if (found == connections_.end() || found->second.endpoint->id() != transportId) return;
    cancelAuthenticationTimer(found->second);
    found->second.authentication = AuthenticationState::Closing;
    (void)sessions_.offline(transportId);
    connections_.erase(found);
}

void GameServerPipeline::onFrames(
    const std::string& connectionName,
    gamenet::transport::TransportSessionId transportId,
    std::vector<std::string> frames) {
    loop_->assertInLoopThread();
    const auto callbackState = callbackState_;
    if (!callbackState->active()) return;
    const auto stageObserver = options_.stageObserver;
    if (stageObserver) stageObserver(GameServerPipelineStage::Management);
    if (!callbackState->active()) return;
    const auto found = connections_.find(connectionName);
    if (found == connections_.end() || found->second.endpoint->id() != transportId) return;
    for (auto& frame : frames) handleFrame(connectionName, std::move(frame));
}

void GameServerPipeline::handleFrame(const std::string& connectionName, std::string payload) {
    auto found = connections_.find(connectionName);
    if (found == connections_.end()) return;
    auto& state = found->second;

    if (state.authentication == AuthenticationState::Unauthenticated) {
        constexpr std::string_view prefix = "AUTH ";
        if (!payload.starts_with(prefix) || payload.size() == prefix.size()) {
            closeConnection(state, gamenet::transport::CloseReason::ProtocolError);
            return;
        }
        state.authentication = AuthenticationState::Authenticating;
        auto endpoint = state.endpoint;
        auto playerId = payload.substr(prefix.size());
        if (options_.authenticationDelay == std::chrono::steady_clock::duration::zero()) {
            beginAuthentication(connectionName, endpoint, std::move(playerId));
        } else {
            const auto callbackState = callbackState_;
            auto attempt = std::make_shared<AuthenticationAttempt>(std::move(playerId));
            state.authenticationAttempt = attempt;
            state.authenticationTimer = loop_->runAfter(
                options_.authenticationDelay,
                [this, callbackState, connectionName, attempt = std::move(attempt)]() mutable {
                    if (!callbackState->active()) return;
                    const auto found = connections_.find(connectionName);
                    if (found == connections_.end() ||
                        found->second.authentication != AuthenticationState::Authenticating ||
                        found->second.authenticationAttempt != attempt) {
                        return;
                    }
                    found->second.authenticationTimer.reset();
                    found->second.authenticationAttempt.reset();
                    beginAuthentication(
                        connectionName, found->second.endpoint, std::move(attempt->playerId));
                });
        }
        return;
    }

    if (state.authentication == AuthenticationState::Authenticating) {
        if (payload.starts_with("AUTH ")) {
            closeConnection(state, gamenet::transport::CloseReason::ProtocolError);
            return;
        }
        if (state.pendingAuthFrames.size() >= options_.maxPendingAuthFrames ||
            payload.size() > options_.maxPendingAuthBytes - state.pendingAuthBytes) {
            closeConnection(state, gamenet::transport::CloseReason::Overloaded);
            return;
        }
        state.pendingAuthBytes += payload.size();
        state.pendingAuthFrames.push_back(std::move(payload));
        return;
    }

    if (state.authentication == AuthenticationState::Online) {
        (void)submitCommand(state, std::move(payload));
    }
}

void GameServerPipeline::beginAuthentication(
    const std::string& connectionName,
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
    std::string playerId) {
    loop_->assertInLoopThread();
    if (!callbackState_->active()) return;
    const auto current = connections_.find(connectionName);
    if (current == connections_.end() || current->second.endpoint->id() != endpoint->id() ||
        current->second.authentication != AuthenticationState::Authenticating) {
        return;
    }
    const auto callbackState = callbackState_;
    sessions_.postAuthenticate(
        std::move(playerId),
        endpoint,
        [this, callbackState, connectionName, endpoint](
            gamenet::game_session::AuthenticateResult result) {
            if (callbackState->active()) {
                completeAuthentication(connectionName, endpoint, std::move(result));
            }
        });
}

void GameServerPipeline::completeAuthentication(
    const std::string& connectionName,
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
    gamenet::game_session::AuthenticateResult result) {
    loop_->assertInLoopThread();
    if (!callbackState_->active()) return;
    const auto current = connections_.find(connectionName);
    if (current == connections_.end() || current->second.endpoint->id() != endpoint->id() ||
        current->second.authentication != AuthenticationState::Authenticating) {
        if (result.session) (void)sessions_.offline(endpoint->id());
        return;
    }
    auto& state = current->second;
    if (result.status == gamenet::game_session::AuthenticateStatus::Rejected || !result.session) {
        closeConnection(state, gamenet::transport::CloseReason::Replaced);
        return;
    }

    state.sessionId = result.session->sessionId();
    state.authentication = AuthenticationState::Online;
    auto pending = std::move(state.pendingAuthFrames);
    state.pendingAuthBytes = 0;
    sendFrame(endpoint, "AUTH_OK");
    for (auto& queuedPayload : pending) {
        if (!submitCommand(state, std::move(queuedPayload))) break;
    }
}

bool GameServerPipeline::submitCommand(ConnectionState& state, std::string payload) {
    if (!callbackState_->active() || !logic_) {
        closeConnection(state, gamenet::transport::CloseReason::GoingAway);
        return false;
    }
    gamenet::game_logic::GameCommand command;
    command.sessionId = state.sessionId;
    command.transportId = state.endpoint->id();
    command.payload = std::move(payload);
    if (logic_->submit(std::move(command)) != gamenet::game_logic::SubmitResult::Accepted) {
        closeConnection(state, gamenet::transport::CloseReason::Overloaded);
        return false;
    }
    return true;
}

void GameServerPipeline::handleLogicOutput(gamenet::game_logic::GameCommand command) {
    loop_->assertInLoopThread();
    if (!callbackState_->active()) return;
    auto session = sessions_.findByTransport(command.transportId);
    if (!session || session->sessionId() != command.sessionId) return;
    sendFrame(session->endpoint(), std::move(command.payload));
}

void GameServerPipeline::runOnLogicLoopAndWait(
    std::function<void()> callback,
    std::atomic<bool>* waitActive) {
    if (logicLoop_->isInLoopThread()) {
        callback();
        return;
    }
    auto completion = std::make_shared<std::promise<void>>();
    auto future = completion->get_future();
    if (!logicExecutor_.tryQueue(
            [callback = std::move(callback), completion = std::move(completion)]() mutable {
                try {
                    callback();
                    completion->set_value();
                } catch (...) {
                    completion->set_exception(std::current_exception());
                }
            })) {
        throw std::logic_error("GameServerPipeline logic loop is unavailable");
    }
    if (waitActive) waitActive->store(true, std::memory_order_release);
    try {
        future.get();
    } catch (...) {
        if (waitActive) waitActive->store(false, std::memory_order_release);
        throw;
    }
    if (waitActive) waitActive->store(false, std::memory_order_release);
}

void GameServerPipeline::closeConnection(
    ConnectionState& state,
    gamenet::transport::CloseReason reason) {
    if (state.authentication == AuthenticationState::Closing) return;
    cancelAuthenticationTimer(state);
    state.authentication = AuthenticationState::Closing;
    state.pendingAuthFrames.clear();
    state.pendingAuthBytes = 0;
    auto endpoint = state.endpoint;
    const auto executor = endpoint->ownerExecutor();
    if (executor.isInOwnerThread()) {
        if (callbackState_->active()) endpoint->close(reason);
    } else {
        const auto callbackState = callbackState_;
        (void)executor.tryQueue([endpoint = std::move(endpoint), callbackState, reason] {
            if (callbackState->active()) endpoint->close(reason);
        });
    }
}

void GameServerPipeline::cancelAuthenticationTimer(ConnectionState& state) {
    if (state.authenticationTimer) {
        loop_->cancel(*state.authenticationTimer);
        state.authenticationTimer.reset();
    }
    state.authenticationAttempt.reset();
}

void GameServerPipeline::sendFrame(
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
    std::string payload) {
    auto frame = encoder_.encode(payload);
    const auto callbackState = callbackState_;
    const auto stageObserver = options_.stageObserver;
    if (!frame) {
        (void)endpoint->ownerExecutor().tryQueue([endpoint, callbackState] {
            if (callbackState->active()) {
                endpoint->close(gamenet::transport::CloseReason::ProtocolError);
            }
        });
        return;
    }
    (void)endpoint->ownerExecutor().tryQueue(
        [endpoint, callbackState, stageObserver, frame = std::move(*frame)] {
            if (!callbackState->active()) return;
            if (stageObserver) stageObserver(GameServerPipelineStage::Endpoint);
            if (!callbackState->active()) return;
            endpoint->send(frame);
        });
}

}  // namespace gamenet::examples
