#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"

#include <any>
#include <algorithm>
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
    std::mutex endpointsMutex;
    std::unordered_map<
        std::uint64_t,
        std::weak_ptr<gamenet::transport::TransportEndpoint>>
        endpoints;

    void registerEndpoint(
        const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint) {
        std::lock_guard lock(endpointsMutex);
        endpoints[endpoint->id().value] = endpoint;
    }
    void unregisterEndpoint(gamenet::transport::TransportSessionId id) {
        std::lock_guard lock(endpointsMutex);
        endpoints.erase(id.value);
    }
    gamenet::DispatchResult requestClose(
        gamenet::transport::TransportSessionId id,
        gamenet::transport::CloseReason reason) noexcept {
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
        {
            std::lock_guard lock(endpointsMutex);
            const auto found = endpoints.find(id.value);
            if (found != endpoints.end()) endpoint = found->second.lock();
        }
        return endpoint ? endpoint->requestClose(reason)
                        : gamenet::DispatchResult::EndpointClosed;
    }
};

struct GameServerPipeline::IoConnectionState {
    std::string connectionName;
    gamenet::protocol::PacketFramer framer;
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint;
    gamenet::net::EventLoopExecutor ownerExecutor;
    std::shared_ptr<CallbackState> callbackState;
    std::function<gamenet::DispatchResult(
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
              .idleTimeout = options_.sessionIdleTimeout,
          }),
      logicLoop_(options_.logicLoop ? options_.logicLoop : loop_),
      logicExecutor_(logicLoop_->executor()),
      nextTransportId_(std::make_shared<std::atomic<std::uint64_t>>(1)) {
    if (options_.ioThreads < 0 ||
        options_.authenticationDelay < std::chrono::steady_clock::duration::zero() ||
        options_.sessionIdleTimeout <= std::chrono::steady_clock::duration::zero() ||
        options_.sessionSweepInterval <= std::chrono::steady_clock::duration::zero() ||
        !logicExecutor_.available()) {
        throw std::invalid_argument(
            "GameServerPipeline requires non-negative IO threads/auth delay and a live logic loop");
    }

    server_.setThreadNum(options_.ioThreads);
    server_.setAdmissionOptions(options_.admissionOptions);
    server_.setConnectionBackpressureOptions(options_.connectionBackpressure);
    if (options_.callbackExceptionHandler) {
        server_.setCallbackExceptionHandler(options_.callbackExceptionHandler);
    }
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
                    [this, callbackState, managementExecutor, endpoint](
                        std::string connectionName,
                        gamenet::transport::TransportSessionId transportId,
                        std::vector<std::string> frames) mutable
                        -> gamenet::DispatchResult {
                        if (!callbackState->active()) {
                            return gamenet::DispatchResult::Shutdown;
                        }
                        const auto posted = managementExecutor.post(
                            [this,
                             callbackState,
                             connectionName = std::move(connectionName),
                             transportId,
                             frames = std::move(frames)]() mutable {
                                if (callbackState->active()) {
                                    onFrames(connectionName, transportId, std::move(frames));
                                }
                            });
                        const auto result = gamenet::dispatchResult(posted);
                        if (result != gamenet::DispatchResult::Accepted) {
                            (void)endpoint->requestClose(
                                result == gamenet::DispatchResult::QueueFull
                                ? gamenet::transport::CloseReason::Overloaded
                                : gamenet::transport::CloseReason::GoingAway);
                        }
                        return result;
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
                              connection,
                              endpoint = std::move(endpoint)]() mutable {
                    if (callbackState->active()) {
                        onConnected(
                            std::move(connectionName),
                            std::move(endpoint),
                            connection);
                    }
                };
                if (managementExecutor.isInOwnerThread()) {
                    admit();
                } else {
                    const auto posted = managementExecutor.post(std::move(admit));
                    if (posted != gamenet::net::PostResult::Accepted) {
                        (void)ioState->endpoint->requestClose(
                            posted == gamenet::net::PostResult::QueueFull
                            ? gamenet::transport::CloseReason::Overloaded
                            : gamenet::transport::CloseReason::GoingAway);
                    }
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
                const auto posted = managementExecutor.post(std::move(disconnect));
                if (posted != gamenet::net::PostResult::Accepted) {
                    callbackState->unregisterEndpoint(ioState->endpoint->id());
                }
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
            options_.logicOptions);
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
                const auto transportId = command.transportId;
                const auto posted = managementExecutor.post(
                    [this, callbackState, command = std::move(command)]() mutable {
                        if (callbackState->active()) handleLogicOutput(std::move(command));
                    });
                if (posted != gamenet::net::PostResult::Accepted) {
                    (void)callbackState->requestClose(
                        transportId,
                        posted == gamenet::net::PostResult::QueueFull
                        ? gamenet::transport::CloseReason::Overloaded
                        : gamenet::transport::CloseReason::GoingAway);
                }
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
    const auto callbackState = callbackState_;
    sessionSweepTimer_ = loop_->runEvery(options_.sessionSweepInterval, [this, callbackState] {
        if (!callbackState->active()) return;
        (void)sessions_.expireIdle();
        for (auto current = connections_.begin(); current != connections_.end();) {
            if (current->second.endpoint->isOpen()) {
                ++current;
                continue;
            }
            cancelAuthenticationTimer(current->second);
            current->second.authentication = AuthenticationState::Closing;
            (void)sessions_.offline(current->second.endpoint->id());
            callbackState->unregisterEndpoint(current->second.endpoint->id());
            current = connections_.erase(current);
        }
    });
    server_.start();
    started_ = true;
}

void GameServerPipeline::stop() {
    (void)stopGracefully(gamenet::net::TcpServerStopOptions{
        .drainTimeout = std::chrono::milliseconds::zero()});
}

gamenet::net::TcpServerStopFuture GameServerPipeline::stopGracefully(
    gamenet::net::TcpServerStopOptions options) {
    loop_->assertInLoopThread();
    if (stopped_) return serverStopFuture_;
    started_ = false;
    stopped_ = true;
    callbackState_->revoke();
    if (sessionSweepTimer_) {
        loop_->cancel(*sessionSweepTimer_);
        sessionSweepTimer_.reset();
    }
    // Revocation above closes all upper-layer admission before Core stops
    // accepting and begins its connection-output drain.
    serverStopFuture_ = server_.stopGracefully(options);

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
    for (auto& [name, state] : connections_) {
        (void)name;
        cancelAuthenticationTimer(state);
        state.authentication = AuthenticationState::Closing;
        state.pendingAuthFrames.clear();
        state.pendingAuthBytes = 0;
        (void)sessions_.offline(state.endpoint->id());
        callbackState_->unregisterEndpoint(state.endpoint->id());
    }
    connections_.clear();
    sessions_.shutdown();
    return serverStopFuture_;
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
            (void)state->endpoint->requestClose(
                gamenet::transport::CloseReason::ProtocolError);
        }
        return;
    }

    if (!result.frames.empty()) {
        const auto delivered = state->deliverFrames(
            state->connectionName, state->endpoint->id(), std::move(result.frames));
        if (delivered != gamenet::DispatchResult::Accepted) {
            state->closing = true;
            (void)state->endpoint->requestClose(
                delivered == gamenet::DispatchResult::QueueFull
                ? gamenet::transport::CloseReason::Overloaded
                : gamenet::transport::CloseReason::GoingAway);
            return;
        }
    }
    if (!result.needsContinuation || state->continuationQueued) return;

    state->continuationQueued = true;
    const auto posted = state->ownerExecutor.post([state] {
            state->continuationQueued = false;
            if (!state->callbackState->active()) {
                state->closing = true;
                return;
            }
            if (!state->closing) {
                handleIoFramerResult(state, state->framer.push({}));
            }
        });
    if (posted != gamenet::net::PostResult::Accepted) {
        state->continuationQueued = false;
        state->closing = true;
        (void)state->endpoint->requestClose(
            posted == gamenet::net::PostResult::QueueFull
            ? gamenet::transport::CloseReason::Overloaded
            : gamenet::transport::CloseReason::GoingAway);
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
            std::vector<std::string> frames) mutable {
            deliver(std::move(frames));
            return gamenet::DispatchResult::Accepted;
        };
    handleIoFramerResult(state, state->framer.push(bytes));
}

void GameServerPipeline::onConnected(
    std::string connectionName,
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
    std::weak_ptr<gamenet::net::TcpConnection> connection) {
    loop_->assertInLoopThread();
    if (stopped_ || !callbackState_->active()) {
        (void)endpoint->requestClose(gamenet::transport::CloseReason::GoingAway);
        return;
    }
    callbackState_->registerEndpoint(endpoint);
    connections_.try_emplace(
        std::move(connectionName),
        std::move(endpoint),
        std::move(connection));
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
    callbackState_->unregisterEndpoint(transportId);
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
        if (!sessions_.heartbeat(state.endpoint->id())) {
            closeConnection(state, gamenet::transport::CloseReason::GoingAway);
            return;
        }
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
    const auto dispatch = sessions_.postAuthenticate(
        std::move(playerId),
        endpoint,
        [this, callbackState, connectionName, endpoint](
            gamenet::game_session::AuthenticateResult result) {
            if (callbackState->active()) {
                completeAuthentication(connectionName, endpoint, std::move(result));
            }
        });
    if (dispatch != gamenet::DispatchResult::Accepted) {
        const auto found = connections_.find(connectionName);
        if (found != connections_.end() &&
            found->second.endpoint->id() == endpoint->id()) {
            closeConnection(
                found->second,
                dispatch == gamenet::DispatchResult::QueueFull
                ? gamenet::transport::CloseReason::Overloaded
                : gamenet::transport::CloseReason::GoingAway);
        }
    }
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
    if (result.dispatch != gamenet::DispatchResult::Accepted ||
        result.status == gamenet::game_session::AuthenticateStatus::Rejected ||
        !result.session) {
        closeConnection(
            state,
            result.dispatch == gamenet::DispatchResult::QueueFull
            ? gamenet::transport::CloseReason::Overloaded
            : gamenet::transport::CloseReason::Replaced);
        return;
    }

    state.sessionId = result.session->sessionId();
    state.binding = result.session->binding();
    if (auto connection = state.connection.lock();
        connection && !server_.tryMarkConnectionAuthenticated(connection)) {
        (void)sessions_.offline(endpoint->id());
        closeConnection(state, gamenet::transport::CloseReason::GoingAway);
        return;
    }
    state.authentication = AuthenticationState::Online;
    auto pending = std::move(state.pendingAuthFrames);
    state.pendingAuthBytes = 0;
    if (sendFrame(endpoint, "AUTH_OK") != gamenet::DispatchResult::Accepted) {
        closeConnection(state, gamenet::transport::CloseReason::Overloaded);
        return;
    }
    for (auto& queuedPayload : pending) {
        if (submitCommand(state, std::move(queuedPayload)) !=
            gamenet::DispatchResult::Accepted) {
            break;
        }
    }
}

gamenet::DispatchResult GameServerPipeline::submitCommand(
    ConnectionState& state,
    std::string payload) {
    if (!callbackState_->active() || !logic_) {
        closeConnection(state, gamenet::transport::CloseReason::GoingAway);
        return gamenet::DispatchResult::Shutdown;
    }
    gamenet::game_logic::GameCommand command;
    command.sessionId = state.sessionId;
    command.transportId = state.endpoint->id();
    command.binding = state.binding;
    command.payload = std::move(payload);
    const auto submitted = logic_->submit(std::move(command));
    if (submitted != gamenet::game_logic::SubmitResult::Accepted) {
        closeConnection(state, gamenet::transport::CloseReason::Overloaded);
        return submitted == gamenet::game_logic::SubmitResult::QueueFull
            ? gamenet::DispatchResult::QueueFull
            : submitted == gamenet::game_logic::SubmitResult::Stopped
                ? gamenet::DispatchResult::Shutdown
                : gamenet::DispatchResult::PolicyRejected;
    }
    return gamenet::DispatchResult::Accepted;
}

void GameServerPipeline::handleLogicOutput(gamenet::game_logic::GameCommand command) {
    loop_->assertInLoopThread();
    if (!callbackState_->active()) return;
    if (command.binding.tracked() && !command.binding.isCurrent()) return;
    auto session = sessions_.findByTransport(command.transportId);
    if (!session || session->sessionId() != command.sessionId ||
        session->binding().generation() != command.binding.generation()) {
        return;
    }
    if (sendFrame(session->endpoint(), std::move(command.payload)) !=
        gamenet::DispatchResult::Accepted) {
        const auto found = std::find_if(
            connections_.begin(),
            connections_.end(),
            [&](const auto& entry) {
                return entry.second.endpoint->id() == command.transportId;
            });
        if (found != connections_.end()) {
            closeConnection(found->second, gamenet::transport::CloseReason::Overloaded);
        }
    }
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
    (void)state.endpoint->requestClose(reason);
}

void GameServerPipeline::cancelAuthenticationTimer(ConnectionState& state) {
    if (state.authenticationTimer) {
        loop_->cancel(*state.authenticationTimer);
        state.authenticationTimer.reset();
    }
    state.authenticationAttempt.reset();
}

gamenet::DispatchResult GameServerPipeline::sendFrame(
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint,
    std::string payload) {
    auto frame = encoder_.encode(payload);
    const auto callbackState = callbackState_;
    const auto stageObserver = options_.stageObserver;
    if (!frame) {
        return endpoint->requestClose(gamenet::transport::CloseReason::ProtocolError);
    }
    const auto posted = endpoint->ownerExecutor().post(
        [endpoint, callbackState, stageObserver, frame = std::move(*frame)] {
            if (!callbackState->active()) return;
            if (stageObserver) stageObserver(GameServerPipelineStage::Endpoint);
            if (!callbackState->active()) return;
            const auto result = endpoint->send(frame);
            if (result != gamenet::transport::EndpointResult::Accepted) {
                (void)endpoint->requestClose(
                    result == gamenet::transport::EndpointResult::Overloaded
                    ? gamenet::transport::CloseReason::Overloaded
                    : gamenet::transport::CloseReason::GoingAway);
            }
        });
    const auto result = gamenet::dispatchResult(posted);
    if (result != gamenet::DispatchResult::Accepted) {
        (void)endpoint->requestClose(
            result == gamenet::DispatchResult::QueueFull
            ? gamenet::transport::CloseReason::Overloaded
            : gamenet::transport::CloseReason::GoingAway);
    }
    return result;
}

}  // namespace gamenet::examples
