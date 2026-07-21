#pragma once

// TcpServer coordinates accepting sockets, assigning connections to loops, and
// base-loop connection bookkeeping. It does not parse application protocols.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/EventLoopThreadPool.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnectionOptions.h"
#include "gamenet/core/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gamenet::net {

class EventLoop;

enum class TcpServerStopOutcome {
    Drained,
    ForcedAfterTimeout,
    ForcedByImmediateStop,
    AlreadyStopped,
    ServerDestroyed,
    SchedulingFailed,
};

struct TcpServerStopOptions {
    std::chrono::milliseconds drainTimeout{5000};

    void validate() const;
};

struct TcpServerStopResult {
    TcpServerStopOutcome outcome{TcpServerStopOutcome::Drained};
    std::size_t initialConnectionCount{0};
    std::size_t forcedConnectionCount{0};
};

using TcpServerStopFuture = std::shared_future<TcpServerStopResult>;

struct TcpServerAdmissionOptions {
    // Zero disables the corresponding limit.
    std::size_t maxConnections{0};
    std::size_t maxConnectionsPerPeer{0};
    std::size_t maxConnectionAttemptsPerPeerPerWindow{0};
    std::chrono::milliseconds connectionAttemptWindow{1000};
    // Bounds abuse-accounting memory when the per-peer rate limit is enabled.
    std::size_t maxTrackedPeerAddresses{65536};
    // Zero disables the authentication deadline.
    std::chrono::milliseconds unauthenticatedTimeout{0};

    void validate() const;
};

enum class TcpServerAdmissionEvent {
    Accepted,
    RejectedConnectionLimit,
    RejectedPerPeerLimit,
    RejectedPerPeerRateLimit,
    RejectedPeerTrackingCapacity,
    Authenticated,
    AuthenticationTimedOut,
};

struct TcpServerAdmissionMetric {
    TcpServerAdmissionEvent event{TcpServerAdmissionEvent::Accepted};
    std::string peerAddress;
    std::string connectionName;
    std::size_t activeConnections{0};
    std::size_t activePeerConnections{0};
};

using TcpServerAdmissionMetricCallback =
    std::function<void(const TcpServerAdmissionMetric&)>;

struct TcpServerAdmissionStats {
    std::uint64_t accepted{0};
    std::uint64_t rejectedConnectionLimit{0};
    std::uint64_t rejectedPerPeerLimit{0};
    std::uint64_t rejectedPerPeerRateLimit{0};
    std::uint64_t rejectedPeerTrackingCapacity{0};
    std::uint64_t authenticated{0};
    std::uint64_t authenticationTimedOut{0};
    std::uint64_t activeConnections{0};
};

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
    void setConnectionBackpressureOptions(TcpConnectionBackpressureOptions options);
    void setAcceptErrorCallback(AcceptorErrorCallback cb);
    void setCallbackExceptionHandler(TcpConnectionCallbackExceptionHandler cb);
    void setAdmissionOptions(TcpServerAdmissionOptions options);
    void setAdmissionMetricCallback(TcpServerAdmissionMetricCallback cb);

    // May be called from any thread. True means the base-loop request was
    // accepted; authentication and deadline races resolve in base-loop order.
    bool tryMarkConnectionAuthenticated(const TcpConnectionPtr& connection);
    TcpServerAdmissionStats admissionStats() const noexcept;

    const InetAddress& listenAddress() const noexcept;
    std::size_t connectionCount() const;

    void start();
    // Compatibility stop: immediately force-close active connections.
    void stop();
    // Completion becomes ready only after connection teardown and worker-loop
    // join have converged. Repeated calls share the first operation/result.
    TcpServerStopFuture stopGracefully(TcpServerStopOptions options = {});

private:
    struct GracefulStopState;

    void stopInLoop();
    void beginGracefulStopInLoop(
        const std::shared_ptr<GracefulStopState>& state,
        TcpServerStopOptions options);
    void forceGracefulStopInLoop(
        const std::shared_ptr<GracefulStopState>& state,
        TcpServerStopOutcome outcome);
    void finishGracefulStopInLoop(
        const std::shared_ptr<GracefulStopState>& state,
        TcpServerStopOutcome outcome);
    std::shared_ptr<GracefulStopState> gracefulStopStateSnapshot() const;
    void newConnection(SocketFd sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);
    bool admitPeer(const std::string& peerAddress);
    void trackAcceptedConnection(
        const TcpConnectionPtr& connection,
        const std::string& peerAddress);
    void markConnectionAuthenticatedInLoop(const TcpConnectionPtr& connection);
    void releaseConnectionAdmission(const std::string& connectionName);
    void clearConnectionAdmission();
    void prunePeerRateBuckets(std::chrono::steady_clock::time_point now);
    void emitAdmissionMetric(
        TcpServerAdmissionEvent event,
        const std::string& peerAddress,
        const std::string& connectionName = {}) noexcept;
    bool forceCloseAllConnections();
    bool forceCloseAllConnectionsForGraceful(
        const std::shared_ptr<GracefulStopState>& state,
        TcpServerStopOutcome outcome);

    EventLoop* loop_;
    std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;
    AcceptorErrorCallback acceptErrorCallback_;
    TcpConnectionCallbackExceptionHandler callbackExceptionHandler_;
    TcpServerAdmissionOptions admissionOptions_;
    TcpServerAdmissionMetricCallback admissionMetricCallback_;
    std::atomic<bool> started_{false};
    bool stopped_{false};
    int nextConnId_{1};
    std::size_t highWaterMark_{0};
    TcpConnectionBackpressureOptions backpressureOptions_;
    std::unordered_map<std::string, TcpConnectionPtr> connections_;

    struct PeerRateBucket {
        std::chrono::steady_clock::time_point expiresAt;
        std::size_t attempts{0};
        std::uint64_t generation{0};
    };
    struct PeerRateExpiry {
        std::chrono::steady_clock::time_point expiresAt;
        std::string peerAddress;
        std::uint64_t generation{0};
    };
    std::unordered_map<std::string, std::size_t> activeConnectionsByPeer_;
    std::unordered_map<std::string, std::string> peerByConnection_;
    std::unordered_map<std::string, TimerId> authenticationTimers_;
    std::unordered_map<std::string, PeerRateBucket> peerRateBuckets_;
    std::deque<PeerRateExpiry> peerRateExpiries_;
    std::uint64_t nextPeerRateGeneration_{1};
    std::atomic<std::uint64_t> acceptedConnections_{0};
    std::atomic<std::uint64_t> rejectedConnectionLimit_{0};
    std::atomic<std::uint64_t> rejectedPerPeerLimit_{0};
    std::atomic<std::uint64_t> rejectedPerPeerRateLimit_{0};
    std::atomic<std::uint64_t> rejectedPeerTrackingCapacity_{0};
    std::atomic<std::uint64_t> authenticatedConnections_{0};
    std::atomic<std::uint64_t> authenticationTimeouts_{0};
    std::atomic<std::uint64_t> activeAdmissionConnections_{0};
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};
    mutable std::mutex gracefulStopMutex_;
    std::shared_ptr<GracefulStopState> gracefulStopState_;
};

}  // namespace gamenet::net
