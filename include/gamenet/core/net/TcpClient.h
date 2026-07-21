#pragma once

// TcpClient coordinates active connect through Connector and owns at most one
// plain TcpConnection.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnectionOptions.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace gamenet::net {

class Connector;
class EventLoop;
enum class ConnectorEvent;

class TcpClient : private gamenet::base::noncopyable {
public:
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);
    ~TcpClient();

    // Repeated calls for the same pending or active connection lifecycle are
    // coalesced before work is marshaled to the owner loop.
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
    void setConnectionBackpressureOptions(TcpConnectionBackpressureOptions options);
    void setCallbackExceptionHandler(TcpConnectionCallbackExceptionHandler cb);

private:
    void connectInLoop(std::uint64_t requestId);
    void disconnectInLoop();
    void stopInLoop();
    void setRetry(bool enabled);
    void setRetryInLoop(bool enabled) noexcept;
    void handleConnectorEvent(ConnectorEvent event);
    void finishTerminalConnectFailure(std::uint64_t requestId);
    void newConnection(SocketFd sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::string name_;
    std::shared_ptr<Connector> connector_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    TcpConnectionCallbackExceptionHandler callbackExceptionHandler_;
    std::atomic<bool> retry_{false};
    std::atomic<std::uint64_t> nextConnectRequestId_{1};
    std::atomic<std::uint64_t> activeConnectRequestId_{0};
    bool connect_{false};
    std::uint64_t connectorRequestId_{0};
    std::uint64_t connectionRequestId_{0};
    std::uint64_t pendingTerminalConnectRequestId_{0};
    int nextConnId_{1};
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;
    TcpConnectionBackpressureOptions backpressureOptions_;
};

}  // namespace gamenet::net
