#pragma once

// TcpClient coordinates active connect through Connector and owns at most one
// plain TcpConnection.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace gamenet::net {

class Connector;
class EventLoop;

class TcpClient : private gamenet::base::noncopyable {
public:
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);
    ~TcpClient();

    void connect();
    void disconnect();
    void stop();

    void enableRetry();
    void disableRetry();
    bool retry() const noexcept;

    const std::string& name() const noexcept;
    EventLoop* getLoop() const noexcept;
    TcpConnectionPtr connection() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);

private:
    void connectInLoop();
    void disconnectInLoop();
    void stopInLoop();
    void setRetry(bool enabled);
    void setRetryInLoop(bool enabled) noexcept;
    void newConnection(SocketFd sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::string name_;
    std::shared_ptr<Connector> connector_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    std::atomic<bool> retry_{false};
    bool connect_{false};
    int nextConnId_{1};
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;
};

}  // namespace gamenet::net
