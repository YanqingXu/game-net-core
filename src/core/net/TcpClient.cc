#include "gamenet/core/net/TcpClient.h"

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "detail/EventLoopLifecycleRegistry.h"

#include <cassert>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace gamenet::net {

namespace {

constexpr unsigned kControlConnect = 1;
constexpr unsigned kControlDisconnect = 2;
constexpr unsigned kControlStop = 3;

}  // namespace

struct TcpClientControl::State {
    std::mutex mutex;
    TcpClient* target{nullptr};
    bool accepting{true};
    unsigned pendingOperation{0};
    std::uint64_t nextGeneration{1};
    std::uint64_t pendingGeneration{0};
    EventLoopLifecycleSource source;
};

TcpClientControl::TcpClientControl(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PostResult TcpClientControl::post(unsigned operation) const noexcept {
    const auto state = state_;
    if (!state) {
        return PostResult::OwnerUnavailable;
    }

    std::lock_guard lock(state->mutex);
    if (!state->accepting || state->target == nullptr) {
        return PostResult::OwnerUnavailable;
    }
    if (state->nextGeneration == 0) {
        return PostResult::Shutdown;
    }

    const std::uint64_t generation = state->nextGeneration++;
    state->pendingOperation = operation;
    state->pendingGeneration = generation;
    const PostResult result = state->source.signal();
    if (result != PostResult::Accepted &&
        state->pendingGeneration == generation) {
        state->pendingOperation = 0;
        state->pendingGeneration = 0;
    }
    return result;
}

PostResult TcpClientControl::tryConnect() const noexcept {
    return post(kControlConnect);
}

PostResult TcpClientControl::tryDisconnect() const noexcept {
    return post(kControlDisconnect);
}

PostResult TcpClientControl::tryStop() const noexcept {
    return post(kControlStop);
}

bool TcpClientControl::available() const noexcept {
    const auto state = state_;
    if (!state) {
        return false;
    }
    std::lock_guard lock(state->mutex);
    return state->accepting && state->target != nullptr;
}

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name)
    : TcpClient(loop, serverAddr, std::move(name), ConnectorOptions{}) {
}

TcpClient::TcpClient(
    EventLoop* loop,
    const InetAddress& serverAddr,
    std::string name,
    ConnectorOptions connectorOptions)
    : loop_(loop),
      ownerExecutor_(loop->executor()),
      name_(std::move(name)),
      connector_(std::make_shared<Connector>(loop, serverAddr, connectorOptions)),
      retry_(connectorOptions.enableRetry),
      pendingTerminalConnectEvent_(ConnectorEvent::ConnectFailed) {
    connectorOptions.validate();
    std::weak_ptr<void> lifetime = lifetimeToken_;
    connector_->setNewConnectionCallback([this, lifetime](SocketFd sockfd) {
        if (!lifetime.lock()) {
            sockets::close(sockfd);
            return;
        }
        newConnection(sockfd);
    });
    connector_->setConnectorEventCallback(
        [this, lifetime](const InetAddress&, ConnectorEvent event) {
            if (lifetime.lock()) {
                handleConnectorEvent(event);
            }
        });

    controlState_ = std::make_shared<TcpClientControl::State>();
    controlState_->target = this;
    const auto controlState = controlState_;
    controlState_->source = detail::EventLoopLifecycleRegistry::attach(
        *loop_,
        [controlState] {
            TcpClient* target = nullptr;
            {
                std::lock_guard lock(controlState->mutex);
                if (controlState->accepting) {
                    target = controlState->target;
                }
            }
            if (target != nullptr) {
                target->driveControlInLoop();
            }
        });
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();
    const auto controlState = controlState_;
    if (controlState) {
        {
            std::lock_guard lock(controlState->mutex);
            controlState->accepting = false;
            controlState->target = nullptr;
            controlState->pendingOperation = 0;
            controlState->pendingGeneration = 0;
        }
        detail::EventLoopLifecycleRegistry::detach(
            *loop_,
            controlState->source);
        controlState_.reset();
    }
    {
        std::lock_guard lock(admissionMutex_);
        lifetimeToken_.reset();
        activeConnectRequestId_.store(0, std::memory_order_release);
        latestAcceptedOperationGeneration_ = nextOperationGeneration_++;
    }
    pendingReconnectRequestId_ = 0;
    pendingTerminalConnectRequestId_ = 0;

    TcpConnectionPtr conn;
    {
        std::lock_guard lock(mutex_);
        conn = connection_;
        connection_.reset();
    }

    if (conn) {
        conn->setCloseCallback([](const TcpConnectionPtr& connection) {
            connection->connectDestroyed();
        });
        conn->getLoop()->runInLoop([conn] {
            if (!conn->disconnected()) {
                conn->forceClose();
                return;
            }
            conn->connectDestroyed();
        });
    }

    if (connector_) {
        connector_->setNewConnectionCallback({});
        connector_->setConnectorEventCallback({});
        connector_->stop();
    }
}

PostResult TcpClient::tryConnect() noexcept {
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
    std::weak_ptr<void> lifetime;
    bool runInline = false;
    try {
        {
            std::lock_guard lock(admissionMutex_);
            const std::uint64_t activeRequest =
                activeConnectRequestId_.load(std::memory_order_acquire);
            if (activeRequest != 0) {
                bool endedLifecycleAwaitingRemoval = false;
                {
                    std::lock_guard connectionLock(mutex_);
                    endedLifecycleAwaitingRemoval =
                        connection_ &&
                        connection_->disconnected() &&
                        connectionRequestId_ == activeRequest &&
                        pendingReconnectRequestId_ == 0;
                }
                if (!endedLifecycleAwaitingRemoval) {
                    return PostResult::Accepted;
                }
            }

            requestId =
                nextConnectRequestId_.fetch_add(1, std::memory_order_relaxed);
            generation = nextOperationGeneration_++;
            lifetime = lifetimeToken_;
            runInline =
                ownerExecutor_.available() &&
                ownerExecutor_.isInOwnerThread();
            if (runInline) {
                activeConnectRequestId_.store(
                    requestId,
                    std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            } else {
                const PostResult result = ownerExecutor_.post(
                    [this, lifetime, requestId, generation] {
                        if (lifetime.lock()) {
                            connectInLoop(requestId, generation);
                        }
                    });
                if (result != PostResult::Accepted) {
                    return result;
                }
                activeConnectRequestId_.store(
                    requestId,
                    std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            }
        }

        if (runInline && lifetime.lock()) {
            connectInLoop(requestId, generation);
        }
        return PostResult::Accepted;
    } catch (const std::exception& error) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline connect operation threw: "
                      << error.what();
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    } catch (...) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline connect operation threw "
                         "a non-standard exception";
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    }
}

PostResult TcpClient::tryDisconnect() noexcept {
    std::uint64_t generation = 0;
    std::weak_ptr<void> lifetime;
    bool runInline = false;
    try {
        {
            std::lock_guard lock(admissionMutex_);
            generation = nextOperationGeneration_++;
            lifetime = lifetimeToken_;
            runInline =
                ownerExecutor_.available() &&
                ownerExecutor_.isInOwnerThread();
            if (runInline) {
                activeConnectRequestId_.store(0, std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            } else {
                const PostResult result = ownerExecutor_.post(
                    [this, lifetime, generation] {
                        if (lifetime.lock()) {
                            disconnectInLoop(generation);
                        }
                    });
                if (result != PostResult::Accepted) {
                    return result;
                }
                activeConnectRequestId_.store(0, std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            }
        }

        if (runInline && lifetime.lock()) {
            disconnectInLoop(generation);
        }
        return PostResult::Accepted;
    } catch (const std::exception& error) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline disconnect operation threw: "
                      << error.what();
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    } catch (...) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline disconnect operation threw "
                         "a non-standard exception";
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    }
}

PostResult TcpClient::tryStop() noexcept {
    std::uint64_t generation = 0;
    std::weak_ptr<void> lifetime;
    bool runInline = false;
    try {
        {
            std::lock_guard lock(admissionMutex_);
            generation = nextOperationGeneration_++;
            lifetime = lifetimeToken_;
            runInline =
                ownerExecutor_.available() &&
                ownerExecutor_.isInOwnerThread();
            if (runInline) {
                activeConnectRequestId_.store(0, std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            } else {
                const PostResult result = ownerExecutor_.post(
                    [this, lifetime, generation] {
                        if (lifetime.lock()) {
                            stopInLoop(generation);
                        }
                    });
                if (result != PostResult::Accepted) {
                    return result;
                }
                activeConnectRequestId_.store(0, std::memory_order_release);
                latestAcceptedOperationGeneration_ = generation;
            }
        }

        if (runInline && lifetime.lock()) {
            stopInLoop(generation);
        }
        return PostResult::Accepted;
    } catch (const std::exception& error) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline stop operation threw: "
                      << error.what();
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    } catch (...) {
        if (runInline) {
            LOG_ERROR << "TcpClient inline stop operation threw "
                         "a non-standard exception";
            return PostResult::Accepted;
        }
        return PostResult::QueueFull;
    }
}

void TcpClient::connect() {
    (void)tryConnect();
}

void TcpClient::disconnect() {
    (void)tryDisconnect();
}

void TcpClient::stop() {
    (void)tryStop();
}

TcpClientControl TcpClient::control() const noexcept {
    return TcpClientControl(controlState_);
}

void TcpClient::enableRetry() {
    (void)tryEnableRetry();
}

void TcpClient::disableRetry() {
    (void)tryDisableRetry();
}

PostResult TcpClient::tryEnableRetry() noexcept {
    return trySetRetry(true);
}

PostResult TcpClient::tryDisableRetry() noexcept {
    return trySetRetry(false);
}

PostResult TcpClient::trySetRetry(bool enabled) noexcept {
    std::weak_ptr<void> lifetime;
    bool runInline = false;
    try {
        {
            std::lock_guard lock(admissionMutex_);
            lifetime = lifetimeToken_;
            runInline =
                ownerExecutor_.available() &&
                ownerExecutor_.isInOwnerThread();
            if (!runInline) {
                return ownerExecutor_.post(
                    [this, lifetime, enabled] {
                        if (lifetime.lock()) {
                            setRetryInLoop(enabled);
                        }
                    });
            }
        }
        if (runInline && lifetime.lock()) {
            setRetryInLoop(enabled);
            return PostResult::Accepted;
        }
        return PostResult::OwnerUnavailable;
    } catch (...) {
        return PostResult::QueueFull;
    }
}

bool TcpClient::retry() const noexcept {
    return retry_.load(std::memory_order_relaxed);
}

const std::string& TcpClient::name() const noexcept {
    return name_;
}

EventLoop* TcpClient::getLoop() const noexcept {
    return loop_;
}

TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard lock(mutex_);
    return connection_;
}

void TcpClient::setConnectionCallback(ConnectionCallback cb) {
    loop_->assertInLoopThread();
    connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
    loop_->assertInLoopThread();
    messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
    loop_->assertInLoopThread();
    writeCompleteCallback_ = std::move(cb);
}

void TcpClient::setCloseInfoCallback(CloseInfoCallback cb) {
    loop_->assertInLoopThread();
    closeInfoCallback_ = std::move(cb);
}

void TcpClient::setConnectionBackpressureOptions(
    TcpConnectionBackpressureOptions options) {
    loop_->assertInLoopThread();
    options.validate();
    if (activeConnectRequestId_.load(std::memory_order_relaxed) != 0) {
        throw std::logic_error(
            "TcpClient backpressure options must be configured before connect");
    }
    backpressureOptions_ = options;
}

void TcpClient::setCallbackExceptionHandler(
    TcpConnectionCallbackExceptionHandler cb) {
    loop_->assertInLoopThread();
    callbackExceptionHandler_ = std::move(cb);
}

void TcpClient::setTerminalConnectFailureCallback(
    TerminalConnectFailureCallback cb) {
    loop_->assertInLoopThread();
    terminalConnectFailureCallback_ = std::move(cb);
}

void TcpClient::driveControlInLoop() {
    loop_->assertInLoopThread();
    const auto controlState = controlState_;
    if (!controlState) {
        return;
    }

    unsigned operation = 0;
    {
        std::lock_guard lock(controlState->mutex);
        if (!controlState->accepting ||
            controlState->target != this) {
            return;
        }
        operation = controlState->pendingOperation;
        controlState->pendingOperation = 0;
        controlState->pendingGeneration = 0;
    }

    switch (operation) {
        case kControlConnect:
            (void)tryConnect();
            break;
        case kControlDisconnect:
            (void)tryDisconnect();
            break;
        case kControlStop:
            (void)tryStop();
            break;
        default:
            break;
    }
}

bool TcpClient::isLatestAcceptedOperation(std::uint64_t generation) const {
    std::lock_guard lock(admissionMutex_);
    return latestAcceptedOperationGeneration_ == generation;
}

void TcpClient::connectInLoop(
    std::uint64_t requestId,
    std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (!isLatestAcceptedOperation(generation)) {
        return;
    }
    if (activeConnectRequestId_.load(std::memory_order_relaxed) != requestId) {
        return;
    }

    connect_ = true;

    {
        std::lock_guard lock(mutex_);
        if (connection_) {
            if (connection_->connected()) {
                // A queued disconnect that never became the latest operation
                // left the current connection active. Rebind that live
                // lifecycle to the latest explicit connect request.
                connectionRequestId_ = requestId;
                pendingReconnectRequestId_ = 0;
            } else {
                // Disconnect has already entered its owner-loop state
                // transition. Preserve this explicit request separately from
                // automatic retry policy until the old close callback removes
                // the connection.
                pendingReconnectRequestId_ = requestId;
            }
            return;
        }
    }

    connectorRequestId_ = requestId;
    if (connector_->state() == Connector::kConnecting) {
        connector_->start();
        return;
    }
    if (connector_->state() == Connector::kConnected) {
        connector_->restart();
        return;
    }
    connector_->start();
}

void TcpClient::disconnectInLoop(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (!isLatestAcceptedOperation(generation)) {
        return;
    }
    connect_ = false;

    TcpConnectionPtr conn;
    {
        std::lock_guard lock(mutex_);
        conn = connection_;
    }

    if (conn) {
        conn->shutdown();
    } else {
        connector_->stop();
    }
}

void TcpClient::stopInLoop(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (!isLatestAcceptedOperation(generation)) {
        return;
    }
    connect_ = false;
    connector_->stop();
}

void TcpClient::setRetryInLoop(bool enabled) noexcept {
    loop_->assertInLoopThread();
    retry_.store(enabled, std::memory_order_relaxed);
    if (connector_) {
        connector_->setRetryEnabled(enabled);
    }
}

void TcpClient::handleConnectorEvent(ConnectorEvent event) {
    loop_->assertInLoopThread();

    if (event == ConnectorEvent::RetryScheduled || event == ConnectorEvent::ConnectSuccess) {
        pendingTerminalConnectRequestId_ = 0;
        return;
    }
    if (event == ConnectorEvent::TerminalFailure) {
        const std::uint64_t requestId = pendingTerminalConnectRequestId_;
        if (requestId != 0) {
            finishTerminalConnectFailure(
                requestId,
                pendingTerminalConnectEvent_);
        }
        return;
    }
    if (event != ConnectorEvent::ConnectFailed &&
        event != ConnectorEvent::SelfConnectDetected &&
        event != ConnectorEvent::ConnectTimeout) {
        return;
    }

    const std::uint64_t requestId = connectorRequestId_;
    if (requestId == 0) {
        return;
    }

    pendingTerminalConnectRequestId_ = requestId;
    pendingTerminalConnectEvent_ = event;
}

void TcpClient::finishTerminalConnectFailure(
    std::uint64_t requestId,
    ConnectorEvent event) {
    loop_->assertInLoopThread();
    if (pendingTerminalConnectRequestId_ != requestId) {
        return;
    }

    pendingTerminalConnectRequestId_ = 0;
    if (connectorRequestId_ == requestId) {
        connectorRequestId_ = 0;
    }

    bool releasedCurrentRequest = false;
    {
        std::lock_guard lock(admissionMutex_);
        std::uint64_t activeRequest = requestId;
        releasedCurrentRequest = activeConnectRequestId_.compare_exchange_strong(
            activeRequest,
            0,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    if (releasedCurrentRequest && terminalConnectFailureCallback_) {
        try {
            terminalConnectFailureCallback_(connector_->serverAddress(), event);
        } catch (const std::exception& error) {
            LOG_ERROR << "TcpClient terminal connect failure callback threw: "
                      << error.what();
        } catch (...) {
            LOG_ERROR << "TcpClient terminal connect failure callback threw "
                         "a non-standard exception";
        }
    }
}

void TcpClient::newConnection(SocketFd sockfd) {
    Socket pendingSocket(sockfd);
    loop_->assertInLoopThread();

    const std::uint64_t requestId = connectorRequestId_;
    if (requestId == 0 ||
        activeConnectRequestId_.load(std::memory_order_relaxed) != requestId) {
        return;
    }

    sockaddr_storage peerStorage{};
    if (!sockets::tryGetPeerAddr(pendingSocket.fd(), &peerStorage)) {
        LOG_ERROR << "TcpClient::newConnection getpeername error: "
                  << sockets::errorMessage(sockets::lastError());
        if (retry_.load(std::memory_order_relaxed) && connect_) {
            connectorRequestId_ = requestId;
            connector_->restart();
        } else {
            pendingTerminalConnectRequestId_ = requestId;
            finishTerminalConnectFailure(
                requestId,
                ConnectorEvent::ConnectFailed);
        }
        return;
    }
    const InetAddress peerAddr(peerStorage);
    sockaddr_storage localStorage{};
    if (!sockets::tryGetLocalAddr(pendingSocket.fd(), &localStorage)) {
        LOG_ERROR << "TcpClient::newConnection getsockname error: "
                  << sockets::errorMessage(sockets::lastError());
        if (retry_.load(std::memory_order_relaxed) && connect_) {
            connectorRequestId_ = requestId;
            connector_->restart();
        } else {
            pendingTerminalConnectRequestId_ = requestId;
            finishTerminalConnectFailure(
                requestId,
                ConnectorEvent::ConnectFailed);
        }
        return;
    }
    const InetAddress localAddr(localStorage);
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);
    const SocketFd connectedFd = pendingSocket.fd();

    auto conn = std::make_shared<TcpConnection>(
        loop_,
        connName,
        connectedFd,
        localAddr,
        peerAddr);
    (void)pendingSocket.releaseFd();
    conn->setBackpressureOptions(backpressureOptions_);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseInfoCallback(closeInfoCallback_);
    conn->setCallbackExceptionHandler(callbackExceptionHandler_);

    std::weak_ptr<void> lifetime = lifetimeToken_;
    conn->setCloseCallback([this, lifetime](const TcpConnectionPtr& connection) {
        if (lifetime.lock()) {
            removeConnection(connection);
        }
    });

    try {
        {
            std::lock_guard lock(mutex_);
            connection_ = conn;
            connectionRequestId_ = requestId;
            pendingReconnectRequestId_ = 0;
        }

#ifdef _WIN32
        // ConnectEx already associated this socket with the owner loop's IOCP.
        // The ledger entry and replacement Channel publication are one
        // rollback-capable transaction.
        loop_->preserveSocketAssociation(connectedFd);
#endif
        conn->connectEstablished();
    } catch (...) {
        const auto handoffFailure = std::current_exception();
        {
            std::lock_guard lock(mutex_);
            if (connection_ == conn) {
                connection_.reset();
                connectionRequestId_ = 0;
                pendingReconnectRequestId_ = 0;
            }
        }

        // A registration failure occurs before kConnected publication, so
        // this cleanup cannot report a disconnected callback for an
        // unpublished connection.
        conn->connectDestroyed();
#ifdef _WIN32
        loop_->forgetSocketAssociation(connectedFd);
#endif
        pendingTerminalConnectRequestId_ = requestId;
        pendingTerminalConnectEvent_ = ConnectorEvent::ConnectFailed;
        finishTerminalConnectFailure(
            requestId,
            ConnectorEvent::ConnectFailed);
        std::rethrow_exception(handoffFailure);
    }
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();

    std::uint64_t requestId = 0;
    std::uint64_t pendingReconnectRequestId = 0;
    {
        std::lock_guard lock(mutex_);
        assert(connection_ == conn);
        connection_.reset();
        requestId = connectionRequestId_;
        connectionRequestId_ = 0;
        pendingReconnectRequestId = pendingReconnectRequestId_;
        pendingReconnectRequestId_ = 0;
    }

    // Explicit socket close permits the numeric handle to be reused
    // immediately. Remove the old Channel before a re-entrant reconnect can
    // create/register a replacement with that same value. EventLoop's active-
    // batch invalidation keeps inline current-Channel removal safe while this
    // callback frame retains the TcpConnection.
    conn->connectDestroyed();

    if (pendingReconnectRequestId != 0 &&
        activeConnectRequestId_.load(std::memory_order_relaxed) ==
            pendingReconnectRequestId &&
        connect_) {
        connectorRequestId_ = pendingReconnectRequestId;
        connector_->restart();
        return;
    }

    if (requestId != 0 &&
        activeConnectRequestId_.load(std::memory_order_relaxed) == requestId &&
        retry_.load(std::memory_order_relaxed) && connect_) {
        connectorRequestId_ = requestId;
        connector_->restart();
        return;
    }

    {
        std::lock_guard lock(admissionMutex_);
        std::uint64_t activeRequest = requestId;
        activeConnectRequestId_.compare_exchange_strong(
            activeRequest,
            0,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
}

}  // namespace gamenet::net
