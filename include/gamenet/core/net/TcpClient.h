#pragma once

// TcpClient coordinates active connect through Connector and owns at most one
// plain TcpConnection.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/ConnectorOptions.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpClientControl.h"
#include "gamenet/core/net/TcpConnectionOptions.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace gamenet::net {

class Connector;
class EventLoop;
enum class ConnectorEvent;

class TcpClient : private gamenet::base::noncopyable {
public:
    using TerminalConnectFailureCallback =
        std::function<void(const InetAddress&, ConnectorEvent)>;

    // loop must be non-null and outlive this client. Construction/destruction
    // and callback/configuration mutation are owner-loop-only.
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);
    TcpClient(
        EventLoop* loop,
        const InetAddress& serverAddr,
        std::string name,
        ConnectorOptions connectorOptions);
    // Destruction is owner-loop-only.
    ~TcpClient();

    // Repeated calls for the same pending or active connection lifecycle are
    // coalesced before work is marshaled to the owner loop. The void lifecycle
    // methods are compatibility wrappers that intentionally discard the typed
    // PostResult returned by their try* counterparts.
    PostResult tryConnect() noexcept;
    PostResult tryDisconnect() noexcept;
    PostResult tryStop() noexcept;
    void connect();
    void disconnect();
    void stop();
    TcpClientControl control() const noexcept;

    // Typed cross-thread retry-policy admission. The void methods below are
    // compatibility wrappers that intentionally discard this result.
    PostResult tryEnableRetry() noexcept;
    PostResult tryDisableRetry() noexcept;
    void enableRetry();
    void disableRetry();
    bool retry() const noexcept;

    const std::string& name() const noexcept;
    EventLoop* getLoop() const noexcept;
    TcpConnectionPtr connection() const;

    // Setters are owner-loop-only. Callback replacements affect future
    // publications on that owner. Backpressure options must be set before an
    // accepted connect lifecycle.
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseInfoCallback(CloseInfoCallback cb);
    void setConnectionBackpressureOptions(TcpConnectionBackpressureOptions options);
    void setCallbackExceptionHandler(TcpConnectionCallbackExceptionHandler cb);
    // Runs synchronously on the owner loop after terminal request cleanup, may
    // re-enter client lifecycle APIs, and has all exceptions logged/contained.
    void setTerminalConnectFailureCallback(TerminalConnectFailureCallback cb);

private:
    void connectInLoop(std::uint64_t requestId, std::uint64_t generation);
    void disconnectInLoop(std::uint64_t generation);
    void stopInLoop(std::uint64_t generation);
    PostResult trySetRetry(bool enabled) noexcept;
    void setRetryInLoop(bool enabled) noexcept;
    void handleConnectorEvent(ConnectorEvent event);
    void finishTerminalConnectFailure(
        std::uint64_t requestId,
        ConnectorEvent event);
    bool isLatestAcceptedOperation(std::uint64_t generation) const;
    void newConnection(SocketFd sockfd);
    void removeConnection(const TcpConnectionPtr& conn);
    void driveControlInLoop();

    EventLoop* loop_;
    EventLoopExecutor ownerExecutor_;
    std::string name_;
    std::shared_ptr<Connector> connector_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseInfoCallback closeInfoCallback_;
    TcpConnectionCallbackExceptionHandler callbackExceptionHandler_;
    TerminalConnectFailureCallback terminalConnectFailureCallback_;
    std::atomic<bool> retry_{false};
    std::atomic<std::uint64_t> nextConnectRequestId_{1};
    std::atomic<std::uint64_t> activeConnectRequestId_{0};
    bool connect_{false};
    std::uint64_t connectorRequestId_{0};
    std::uint64_t connectionRequestId_{0};
    std::uint64_t pendingReconnectRequestId_{0};
    std::uint64_t pendingTerminalConnectRequestId_{0};
    ConnectorEvent pendingTerminalConnectEvent_;
    mutable std::mutex admissionMutex_;
    std::uint64_t nextOperationGeneration_{1};
    std::uint64_t latestAcceptedOperationGeneration_{0};
    int nextConnId_{1};
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;
    TcpConnectionBackpressureOptions backpressureOptions_;
    std::shared_ptr<TcpClientControl::State> controlState_;
};

}  // namespace gamenet::net
