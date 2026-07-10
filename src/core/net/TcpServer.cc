#include "gamenet/core/net/TcpServer.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace gamenet::net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, reusePort)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_)) {
    acceptor_->setNewConnectionCallback(
        [this](SocketFd sockfd, const InetAddress& peerAddr) { newConnection(sockfd, peerAddr); });
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    lifetimeToken_.reset();
    stopInLoop();
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::setThreadInitCallback(ThreadInitCallback cb) {
    threadInitCallback_ = std::move(cb);
}

void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpServer::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpServer::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

const InetAddress& TcpServer::listenAddress() const noexcept {
    return acceptor_->listenAddress();
}

std::size_t TcpServer::connectionCount() const {
    loop_->assertInLoopThread();
    return connections_.size();
}

void TcpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    stopped_ = false;
    threadPool_->start(threadInitCallback_);
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            acceptor_->listen();
        }
    });
}

void TcpServer::stop() {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (lifetime.lock()) {
            stopInLoop();
        }
    });
}

void TcpServer::stopInLoop() {
    loop_->assertInLoopThread();
    if (stopped_) {
        return;
    }

    stopped_ = true;
    acceptor_->setNewConnectionCallback({});
    if (acceptor_->listening()) {
        acceptor_->stop();
    }
    if (!forceCloseAllConnections()) {
        threadPool_->stop();
    }
}

void TcpServer::newConnection(SocketFd sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();
    if (stopped_) {
        sockets::close(sockfd);
        return;
    }

    EventLoop* ioLoop = threadPool_->getNextLoop();
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);
    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    auto connection = std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);
    connections_[connName] = connection;

    std::weak_ptr<void> lifetime = lifetimeToken_;
    CloseCallback closeCallback = [this, lifetime](const TcpConnectionPtr& conn) {
        if (lifetime.lock()) {
            removeConnection(conn);
        }
    };

    ioLoop->runInLoop(
        [connection,
         connectionCallback = connectionCallback_,
         messageCallback = messageCallback_,
         highWaterMarkCallback = highWaterMarkCallback_,
         writeCompleteCallback = writeCompleteCallback_,
         closeCallback = std::move(closeCallback),
         highWaterMark = highWaterMark_]() mutable {
            connection->setConnectionCallback(std::move(connectionCallback));
            connection->setMessageCallback(std::move(messageCallback));
            if (highWaterMarkCallback && highWaterMark > 0) {
                connection->setHighWaterMarkCallback(std::move(highWaterMarkCallback), highWaterMark);
            }
            connection->setWriteCompleteCallback(std::move(writeCompleteCallback));
            connection->setCloseCallback(std::move(closeCallback));
            connection->connectEstablished();
        });
}

void TcpServer::removeConnection(const TcpConnectionPtr& connection) {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime, connection] {
        if (lifetime.lock()) {
            removeConnectionInLoop(connection);
        }
    });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    const auto erased = connections_.erase(connection->name());
    if (erased == 0) {
        return;
    }

    EventLoop* connectionLoop = connection->getLoop();
    connectionLoop->queueInLoop([connection] { connection->connectDestroyed(); });
}

bool TcpServer::forceCloseAllConnections() {
    auto connections = std::move(connections_);
    connections_.clear();
    if (connections.empty()) {
        return false;
    }

    auto remaining = std::make_shared<std::atomic<std::size_t>>(connections.size());
    std::weak_ptr<void> lifetime = lifetimeToken_;
    EventLoop* baseLoop = loop_;
    EventLoopThreadPool* threadPool = threadPool_.get();
    auto notifyClosed = [remaining, lifetime, baseLoop, threadPool] {
        if (remaining->fetch_sub(1) != 1) {
            return;
        }

        baseLoop->runInLoop([lifetime, threadPool] {
            if (lifetime.lock()) {
                threadPool->stop();
            }
        });
    };

    for (auto& [name, connection] : connections) {
        (void)name;
        EventLoop* connectionLoop = connection->getLoop();
        connectionLoop->runInLoop([connection, notifyClosed] {
            connection->setCloseCallback([notifyClosed](const TcpConnectionPtr& conn) {
                conn->connectDestroyed();
                notifyClosed();
            });
            if (!connection->disconnected()) {
                connection->forceClose();
                return;
            }
            connection->connectDestroyed();
            notifyClosed();
        });
    }

    return true;
}

}  // namespace gamenet::net
