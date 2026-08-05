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

#include <atomic>
#include <chrono>
#include <cstdint>
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
    TerminalFailure,
};

using ConnectorEventCallback = std::function<void(const InetAddress&, ConnectorEvent)>;

class Connector : public std::enable_shared_from_this<Connector>, private gamenet::base::noncopyable {
public:
    // Runs synchronously on the owner loop and may re-enter lifecycle methods.
    // fd ownership transfers at callback entry; receiver RAII remains
    // responsible if it throws. Connector logs/contains the exception and
    // settles the completed attempt to disconnected without reclaiming fd.
    using NewConnectionCallback = std::function<void(SocketFd sockfd)>;
    using Duration = std::chrono::steady_clock::duration;

    enum StateE { kDisconnected, kConnecting, kConnected };

    // loop must be non-null and outlive this Connector. Construction and
    // destruction are owner-loop-only.
    Connector(EventLoop* loop, const InetAddress& serverAddr);
    Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options);
    ~Connector();

    // Owner-loop-only. The fd becomes callback-owned at callback entry; the
    // receiver must install RAII ownership before any fallible work.
    void setNewConnectionCallback(NewConnectionCallback cb);

    // Owner-loop-only diagnostic observer. It executes synchronously, may
    // re-enter lifecycle methods, and thrown exceptions are contained.
    void setConnectorEventCallback(ConnectorEventCallback cb);

    const InetAddress& serverAddress() const noexcept;
    StateE state() const noexcept;

    /// Start connecting. Owner-loop-thread only.
    void start();

    /// Stop connecting or cancel pending retry. Owner-loop-thread only.
    void stop();

    /// Restart connecting (reset backoff). Owner-loop-thread only.
    void restart();

    /// Configure retry backoff parameters. Owner-loop-only and before first start().
    void setRetryDelay(Duration initial, Duration max);
    void setRetryEnabled(bool enabled);
    bool retryEnabled() const noexcept;

private:
    void startInLoop(std::uint64_t generation);
    void stopInLoop();
    void connect(std::uint64_t generation);
    void connecting(SocketFd sockfd, std::uint64_t generation);
    void handleWrite(std::uint64_t generation);
    void handleError(std::uint64_t generation);
    void handleConnectTimeout(std::uint64_t generation);
    void emitEvent(ConnectorEvent event) noexcept;
    void retry(SocketFd sockfd, std::uint64_t generation);
#ifdef _WIN32
    bool cancelPendingConnectInLoop(SocketFd sockfd) noexcept;
    void finishCancelInLoop();
#endif
    SocketFd removeAndReleaseChannel();

    EventLoop* loop_;
    InetAddress serverAddr_;
    std::atomic<StateE> state_;
    bool connect_;
    std::atomic<bool> retryEnabled_;
    NewConnectionCallback newConnectionCallback_;
    ConnectorEventCallback connectorEventCallback_;
    std::unique_ptr<Channel> channel_;
#ifdef _WIN32
    struct IocpConnectState;
    std::shared_ptr<IocpConnectState> iocpConnect_;
    std::shared_ptr<Connector> connectStopGuard_;
#endif
    Duration retryDelayMs_;
    Duration initialRetryDelay_;
    Duration maxRetryDelayMs_;
    Duration connectTimeout_;
    TimerId retryTimerId_;
    TimerId connectTimeoutTimerId_;
    std::uint64_t requestGeneration_{0};
    bool startedOnce_{false};
};

}  // namespace gamenet::net
