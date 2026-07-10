#include "gamenet/core/net/TcpClient.h"

#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace gamenet::net {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      connector_(std::make_shared<Connector>(loop, serverAddr)) {
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
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();
    lifetimeToken_.reset();
    activeConnectRequestId_.store(0, std::memory_order_relaxed);
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

void TcpClient::connect() {
    const std::uint64_t requestId = nextConnectRequestId_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t idleRequest = 0;
    if (!activeConnectRequestId_.compare_exchange_strong(
            idleRequest,
            requestId,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime, requestId] {
        if (lifetime.lock()) {
            connectInLoop(requestId);
        }
    });
}

void TcpClient::disconnect() {
    activeConnectRequestId_.store(0, std::memory_order_relaxed);
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            disconnectInLoop();
        }
    });
}

void TcpClient::stop() {
    activeConnectRequestId_.store(0, std::memory_order_relaxed);
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            stopInLoop();
        }
    });
}

void TcpClient::enableRetry() {
    setRetry(true);
}

void TcpClient::disableRetry() {
    setRetry(false);
}

void TcpClient::setRetry(bool enabled) {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime, enabled] {
        if (lifetime.lock()) {
            setRetryInLoop(enabled);
        }
    });
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
    connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpClient::connectInLoop(std::uint64_t requestId) {
    loop_->assertInLoopThread();
    if (activeConnectRequestId_.load(std::memory_order_relaxed) != requestId) {
        return;
    }

    connect_ = true;

    {
        std::lock_guard lock(mutex_);
        if (connection_) {
            connectionRequestId_ = requestId;
            return;
        }
    }

    if (connector_->state() == Connector::kConnecting) {
        return;
    }
    connectorRequestId_ = requestId;
    if (connector_->state() == Connector::kConnected) {
        connector_->restart();
        return;
    }
    connector_->start();
}

void TcpClient::disconnectInLoop() {
    loop_->assertInLoopThread();
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

void TcpClient::stopInLoop() {
    loop_->assertInLoopThread();
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
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->queueInLoop([this, lifetime, requestId] {
        if (lifetime.lock()) {
            finishTerminalConnectFailure(requestId);
        }
    });
}

void TcpClient::finishTerminalConnectFailure(std::uint64_t requestId) {
    loop_->assertInLoopThread();
    if (pendingTerminalConnectRequestId_ != requestId) {
        return;
    }

    pendingTerminalConnectRequestId_ = 0;
    if (connectorRequestId_ == requestId) {
        connectorRequestId_ = 0;
    }

    std::uint64_t activeRequest = requestId;
    activeConnectRequestId_.compare_exchange_strong(
        activeRequest,
        0,
        std::memory_order_relaxed,
        std::memory_order_relaxed);
}

void TcpClient::newConnection(SocketFd sockfd) {
    loop_->assertInLoopThread();

    const std::uint64_t requestId = connectorRequestId_;
    if (requestId == 0 ||
        activeConnectRequestId_.load(std::memory_order_relaxed) != requestId) {
        sockets::close(sockfd);
        return;
    }

    const InetAddress peerAddr(sockets::getPeerAddr(sockfd));
    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);

    auto conn = std::make_shared<TcpConnection>(loop_, connName, sockfd, localAddr, peerAddr);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    std::weak_ptr<void> lifetime = lifetimeToken_;
    conn->setCloseCallback([this, lifetime](const TcpConnectionPtr& connection) {
        if (lifetime.lock()) {
            removeConnection(connection);
        }
    });

    {
        std::lock_guard lock(mutex_);
        connection_ = conn;
        connectionRequestId_ = requestId;
    }

    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();

    std::uint64_t requestId = 0;
    {
        std::lock_guard lock(mutex_);
        assert(connection_ == conn);
        connection_.reset();
        requestId = connectionRequestId_;
        connectionRequestId_ = 0;
    }

    loop_->queueInLoop([conn] { conn->connectDestroyed(); });

    if (requestId != 0 &&
        activeConnectRequestId_.load(std::memory_order_relaxed) == requestId &&
        retry_.load(std::memory_order_relaxed) && connect_) {
        connectorRequestId_ = requestId;
        connector_->restart();
        return;
    }

    std::uint64_t activeRequest = requestId;
    activeConnectRequestId_.compare_exchange_strong(
        activeRequest,
        0,
        std::memory_order_relaxed,
        std::memory_order_relaxed);
}

}  // namespace gamenet::net
