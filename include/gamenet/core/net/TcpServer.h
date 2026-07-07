#pragma once

// TcpServer coordinates accepting sockets, assigning connections to loops, and
// base-loop connection bookkeeping. It does not parse application protocols.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/EventLoopThreadPool.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace gamenet::net {

class EventLoop;

class TcpServer : private gamenet::base::noncopyable {
public:
    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort = true);
    ~TcpServer();

    void setThreadNum(int numThreads);
    void setThreadInitCallback(ThreadInitCallback cb);
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);

    const InetAddress& listenAddress() const noexcept;
    std::size_t connectionCount() const;

    void start();
    void stop();

private:
    void stopInLoop();
    void newConnection(SocketFd sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);
    bool forceCloseAllConnections();

    EventLoop* loop_;
    std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;
    std::atomic<bool> started_{false};
    bool stopped_{false};
    int nextConnId_{1};
    std::size_t highWaterMark_{0};
    std::unordered_map<std::string, TcpConnectionPtr> connections_;
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};
};

}  // namespace gamenet::net
