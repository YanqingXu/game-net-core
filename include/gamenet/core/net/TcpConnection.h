#pragma once

// TcpConnection owns the per-connection socket/channel/buffer state for one
// plain TCP connection. It is bound to exactly one EventLoop.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnectionClose.h"
#include "gamenet/core/net/TcpConnectionOptions.h"
#include "gamenet/core/net/TcpOutputMemoryBudget.h"

#include <any>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace gamenet::net {

class Channel;
class EventLoop;
class EventLoopLifecycleSource;
class TcpServer;
namespace detail {
class ConnectionBackpressureController;
}
#ifdef _WIN32
class IocpTcpTransport;
#endif
class Socket;

struct TcpConnectionMemoryRetentionSnapshot {
    BufferRetentionSnapshot inputBuffer;
    BufferRetentionSnapshot outputBuffer;
    std::size_t transportReadStorageBytes{};
    std::size_t peakTransportReadStorageBytes{};
    std::size_t totalRetainedBytes{};
};

class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private gamenet::base::noncopyable {
public:
    enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

    TcpConnection(
        EventLoop* loop,
        std::string name,
        SocketFd sockfd,
        const InetAddress& localAddr,
        const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const noexcept;
    const std::string& name() const noexcept;
    const InetAddress& localAddress() const noexcept;
    const InetAddress& peerAddress() const noexcept;

    // These methods are cross-thread-safe state snapshots. They do not make
    // any other connection-owned state safe to access outside the owner loop.
    bool connected() const noexcept;
    bool disconnected() const noexcept;

    void send(std::string_view message);
    void send(const void* data, std::size_t len);
    // Admission is bounded across both owner-thread buffered bytes and
    // cross-thread payloads accepted but not yet executed by the owner loop.
    TcpSendResult trySend(std::string_view message);
    TcpSendResult trySend(const void* data, std::size_t len);
    void shutdown();
    void forceClose();
    PostResult tryShutdown();
    PostResult tryForceClose();

    // Socket options, context, and callback slots are owner-loop-only mutable
    // state. Configure callbacks before connectEstablished(); only teardown
    // code running on the owner loop may replace a callback afterward.
    void setTcpNoDelay(bool on);
    // Owner-loop setup operation; configure before connectEstablished().
    void setBackpressureOptions(TcpConnectionBackpressureOptions options);

    // Cross-thread-safe snapshot of admitted bytes not yet written or dropped.
    std::size_t pendingOutputBytes() const noexcept;
    // Connection-local capacity/rejection snapshot. Upper loop/server/global
    // scopes are exposed by TcpServer::outputMemoryStats().
    TcpOutputMemoryBudgetSnapshot outputMemorySnapshot() const noexcept;
    // Owner-loop-only low-frequency snapshot of connection-owned retained
    // storage. Cross-thread callers must marshal through the owner executor.
    TcpConnectionMemoryRetentionSnapshot memoryRetentionSnapshot() const;
    // Cross-thread-safe diagnostic count of optional high-water/write-complete
    // notifications dropped because owner-loop queue admission failed.
    std::uint64_t droppedNotificationCount() const noexcept;
    std::optional<TcpConnectionCloseInfo> closeInfo() const noexcept;
    TcpConnectionClosePhase closePhase() const noexcept;
    bool socketClosed() const noexcept;
    // Owner-loop-only diagnostic used by policy/contract integration.
    bool readingPausedByBackpressure() const;

    // Connection context is loop-owned mutable state. Call setContext(), getContext()
    // only from this connection's owner EventLoop thread.
    void setContext(std::any context);
    const std::any& getContext() const;
    std::any& getContext();

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseCallback(CloseCallback cb);
    void setCloseInfoCallback(CloseInfoCallback cb);
    void setCallbackExceptionHandler(TcpConnectionCallbackExceptionHandler cb);

    void connectEstablished();
    void connectDestroyed();

private:
    void handleRead(gamenet::base::Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError(int savedErrno = 0);

#ifdef _WIN32
    void sendReservedInLoop(std::string payload);
#else
    void sendReservedInLoop(const char* data, std::size_t len);
#endif
    void shutdownInLoop();
    void forceCloseInLoop();
    void driveLifecycleInLoop();
    void beginCloseInLoop();
    void finishClose();
    void publishCloseInfo(
        TcpConnectionCloseReason reason,
        int nativeError = 0) noexcept;
    PostResult signalLifecycle() noexcept;
    void detachLifecycleNode();
    void queueWriteComplete() noexcept;
    void maybeQueueHighWaterMark(std::size_t oldLen, std::size_t newLen) noexcept;
    void recordDroppedNotification() noexcept;
    TcpSendResult tryReserveOutputBytes(std::size_t bytes) noexcept;
    void releaseOutputBytes(std::size_t bytes) noexcept;
    void releaseConnectionOutputBytes(std::size_t bytes) noexcept;
    void setOutputMemoryBudgets(
        std::shared_ptr<TcpOutputMemoryBudget> loopBudget,
        std::shared_ptr<TcpOutputMemoryBudget> serverBudget,
        std::shared_ptr<TcpOutputMemoryBudget> globalBudget);
    std::size_t bufferedOutputBytesInLoop() const noexcept;
    void clearBufferedOutputInLoop();
    void applyBackpressureInLoop();
    std::size_t remainingInputCapacity() const noexcept;
    bool closeOnInputLimitInLoop();
    void reportCallbackException(
        TcpConnectionCallbackSource source,
        std::exception_ptr exception) noexcept;
#ifdef _WIN32
    void resumeWindowsReadAfterBackpressure();
#endif
    void setState(StateE state) noexcept;

    EventLoop* loop_;
    std::string name_;
    std::atomic<StateE> state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    std::unique_ptr<detail::ConnectionBackpressureController> backpressure_;
#ifdef _WIN32
    std::unique_ptr<IocpTcpTransport> iocpTransport_;
#endif
    InetAddress localAddr_;
    InetAddress peerAddr_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    CloseInfoCallback closeInfoCallback_;
    TcpConnectionCallbackExceptionHandler callbackExceptionHandler_;
    std::size_t highWaterMark_{0};
    TcpConnectionBackpressureOptions backpressureOptions_;
    std::atomic<std::size_t> pendingOutputBytes_{0};
    std::atomic<std::size_t> peakPendingOutputBytes_{0};
    std::atomic<std::uint64_t> rejectedOutputReservations_{0};
    std::atomic<bool> outputAdmissionOverloaded_{false};
    std::shared_ptr<TcpOutputMemoryBudget> loopOutputBudget_;
    std::shared_ptr<TcpOutputMemoryBudget> serverOutputBudget_;
    std::shared_ptr<TcpOutputMemoryBudget> globalOutputBudget_;
    std::atomic<std::uint64_t> droppedNotificationCount_{0};
    std::atomic<std::uint64_t> closeInfoBits_{0};
    std::atomic<TcpConnectionClosePhase> closePhase_{
        TcpConnectionClosePhase::Open};
    std::atomic<bool> gracefulShutdownRequested_{false};
    std::atomic<bool> forceCloseRequested_{false};
    std::any context_;
    bool channelAdded_{false};
    bool channelRemoved_{false};
    bool forceClosePending_{false};
    TcpConnectionPtr forceCloseGuard_;
    mutable std::mutex lifecycleSourceMutex_;
    std::shared_ptr<EventLoopLifecycleSource> lifecycleSource_;

    friend class TcpServer;
};

}  // namespace gamenet::net
