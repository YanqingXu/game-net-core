#pragma once

// Connector 是主动连接适配器，与 Acceptor 对称。
// 它负责发起非阻塞 connect、处理 EINPROGRESS、检测连接就绪，
// 并将已连接的 fd 通过回调交付给上层。所有 Channel 操作在 owner loop 线程。
// 支持 ConnectorOptions 配置、连接超时、ConnectorEvent hook。

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/ConnectorOptions.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TimerId.h"

#include <chrono>
#include <functional>
#include <memory>

namespace gamenet::net {

class Channel;
class EventLoop;

enum class ConnectorEvent {
    ConnectAttempt,
    ConnectSuccess,
    ConnectFailed,
    RetryScheduled,
    SelfConnectDetected,
    ConnectTimeout,
};

using ConnectorEventCallback = std::function<void(const InetAddress&, ConnectorEvent)>;

class Connector : public std::enable_shared_from_this<Connector>, private gamenet::base::noncopyable {
public:
    using NewConnectionCallback = std::function<void(SocketFd sockfd)>;
    using Duration = std::chrono::steady_clock::duration;

    enum StateE { kDisconnected, kConnecting, kConnected };

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options);
    ~Connector();

    void setNewConnectionCallback(NewConnectionCallback cb);

    /// 设置 ConnectorEvent hook。必须在 start() 前调用。
    void setConnectorEventCallback(ConnectorEventCallback cb);

    const InetAddress& serverAddress() const noexcept;
    StateE state() const noexcept;

    /// Start connecting. Marshals to the owner EventLoop.
    void start();

    /// Stop connecting or cancel pending retry. Owner-loop-thread only.
    void stop();

    /// Restart connecting (reset backoff). Owner-loop-thread only.
    void restart();

    /// Configure retry backoff parameters. Must be set before start().
    void setRetryDelay(Duration initial, Duration max);
    void setRetryEnabled(bool enabled) noexcept;
    bool retryEnabled() const noexcept;

private:
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(SocketFd sockfd);
    void handleWrite();
    void handleError();
    void handleConnectTimeout();
    void retry(SocketFd sockfd);
#ifdef _WIN32
    bool cancelPendingConnectInLoop(SocketFd sockfd) noexcept;
    void finishCancelInLoop();
#endif
    SocketFd removeAndReleaseChannel();

    EventLoop* loop_;
    InetAddress serverAddr_;
    StateE state_;
    bool connect_;
    bool retryEnabled_;
    NewConnectionCallback newConnectionCallback_;
    ConnectorEventCallback connectorEventCallback_;
    std::unique_ptr<Channel> channel_;
#ifdef _WIN32
    struct IocpConnectState;
    std::unique_ptr<IocpConnectState> iocpConnect_;
    std::shared_ptr<Connector> connectStopGuard_;
#endif
    Duration retryDelayMs_;
    Duration maxRetryDelayMs_;
    Duration connectTimeout_;
    TimerId retryTimerId_;
    TimerId connectTimeoutTimerId_;

    static constexpr Duration kDefaultInitRetryDelay = std::chrono::milliseconds(500);
    static constexpr Duration kDefaultMaxRetryDelay = std::chrono::seconds(30);
};

}  // namespace gamenet::net
