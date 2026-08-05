#pragma once

// TcpServer coordinates accepting sockets, assigning connections to loops, and
// base-loop connection bookkeeping. It does not parse application protocols.

#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Callbacks.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/DeadlineQueue.h"
#include "gamenet/core/net/EventLoopThreadPool.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/TcpConnectionOptions.h"
#include "gamenet/core/net/TcpOutputMemoryBudget.h"
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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gamenet::net {

class EventLoop;
class EventLoopLifecycleSource;

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

    // drainTimeout must be non-negative; invalid values throw
    // std::invalid_argument.
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
    std::chrono::milliseconds authenticationDeadlineResolution{10};
    std::size_t maxAuthenticationTimeoutsPerAdvance{1024};

    // Enabled rate limiting requires a positive window and peer-table
    // capacity. Authentication timeout is non-negative; deadline resolution
    // and per-advance budget are positive. Violations throw invalid_argument.
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

struct TcpServerOutputMemoryOptions {
    TcpOutputMemoryBudgetOptions loop{
        .hardLimitBytes = 64U * 1024U * 1024U,
        .recoveryThresholdBytes = 48U * 1024U * 1024U,
    };
    TcpOutputMemoryBudgetOptions server{
        .hardLimitBytes = 256U * 1024U * 1024U,
        .recoveryThresholdBytes = 192U * 1024U * 1024U,
    };
    // Optional process/global scope shared by one or more TcpServer objects.
    std::shared_ptr<TcpOutputMemoryBudget> global;

    // Validates both finite loop/server budgets; configure before start.
    void validate() const;
};

struct TcpServerOutputMemoryStats {
    std::vector<TcpOutputMemoryBudgetSnapshot> loops;
    TcpOutputMemoryBudgetSnapshot server;
    std::optional<TcpOutputMemoryBudgetSnapshot> global;
};

class TcpServer : private gamenet::base::noncopyable {
public:
    // loop must be non-null and must outlive the server. Construction and
    // destruction are base-loop-only.
    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort = true);
    // Destruction is base-loop-only. Connection, message, water-mark, write,
    // close-info and admission callbacks run on their documented owner loops,
    // may re-enter lifecycle APIs, and follow the installed exception policy.
    ~TcpServer();

    // All setters are base-loop-only setup operations and throw
    // std::logic_error after start. Option validation throws
    // std::invalid_argument before state mutation.
    void setThreadNum(int numThreads);
    void setLoopSelectionPolicy(EventLoopSelectionPolicy policy);
    // Uses ThreadInitCallback's worker-handshake/zero-worker synchronous
    // contract; an exception propagates from start() after pool rollback.
    void setThreadInitCallback(ThreadInitCallback cb);
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseInfoCallback(CloseInfoCallback cb);
    void setConnectionBackpressureOptions(TcpConnectionBackpressureOptions options);
    void setOutputMemoryOptions(TcpServerOutputMemoryOptions options);
    // Uses AcceptorErrorCallback's base-loop, re-entry, default Retry, and
    // throw-to-Stop contract.
    void setAcceptErrorCallback(AcceptorErrorCallback cb);
    void setIocpAcceptDepth(std::size_t depth);
    void setCallbackExceptionHandler(TcpConnectionCallbackExceptionHandler cb);
    void setAdmissionOptions(TcpServerAdmissionOptions options);
    // Runs synchronously on the base loop after the corresponding admission
    // state mutation, may re-enter base-loop-safe APIs, and contains/logs all
    // callback exceptions without rolling back the state change.
    void setAdmissionMetricCallback(TcpServerAdmissionMetricCallback cb);

    // May be called from any thread. Accepted means the base-loop request was
    // committed; authentication and deadline races resolve in base-loop order.
    // A null connection returns OwnerUnavailable.
    PostResult tryMarkConnectionAuthenticated(
        const TcpConnectionPtr& connection) noexcept;
    TcpServerAdmissionStats admissionStats() const noexcept;
    TcpServerOutputMemoryStats outputMemoryStats() const;

    const InetAddress& listenAddress() const noexcept;
    std::size_t connectionCount() const;

    // start() and destruction are base-loop-only. start() is idempotent.
    void start();
    // Compatibility stop: immediately force-close active connections and
    // intentionally discard the internal typed lifecycle-signal result. Use
    // stopGracefully() when a terminal outcome must be observed.
    void stop();
    // Completion becomes ready only after connection teardown and worker-loop
    // join have converged. Repeated calls share the first operation/result.
    TcpServerStopFuture stopGracefully(TcpServerStopOptions options = {});

private:
    struct GracefulStopState;
    struct AggregateStopState;
    struct WorkerStopParticipant;

    void assertConfigurable(const char* operation) const;

    void stopInLoop();
    void driveStopLifecycleInLoop();
    void registerWorkerStopParticipant(EventLoop* workerLoop);
    void beginAggregateStopInLoop(
        const std::shared_ptr<GracefulStopState>& gracefulState,
        TcpServerStopOutcome outcome,
        bool force);
    void requestAggregateForceInLoop(TcpServerStopOutcome outcome);
    void consumeWorkerStopNotificationsInLoop();
    void finishAggregateStopInLoop();
    void releaseWorkerConnectionsInLoop(
        const std::shared_ptr<WorkerStopParticipant>& participant,
        std::uint64_t generation);
    PostResult signalStopLifecycle() noexcept;
    static void driveWorkerStopParticipant(
        const std::shared_ptr<WorkerStopParticipant>& participant);
    static void workerConnectionClosed(
        const std::shared_ptr<WorkerStopParticipant>& participant,
        std::uint64_t generation,
        const TcpConnectionPtr& connection);
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
        const std::string& peerAddress,
        DeadlineKey deadlineKey);
    void ensureAuthenticationDeadlineDriver();
    void driveAuthenticationDeadlines();
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
    CloseInfoCallback closeInfoCallback_;
    ThreadInitCallback threadInitCallback_;
    AcceptorErrorCallback acceptErrorCallback_;
    TcpConnectionCallbackExceptionHandler callbackExceptionHandler_;
    TcpServerAdmissionOptions admissionOptions_;
    TcpServerAdmissionMetricCallback admissionMetricCallback_;
    std::atomic<bool> started_{false};
    bool stopped_{false};
    std::uint64_t nextConnId_{1};
    std::size_t highWaterMark_{0};
    TcpConnectionBackpressureOptions backpressureOptions_;
    TcpServerOutputMemoryOptions outputMemoryOptions_;
    std::shared_ptr<TcpOutputMemoryBudget> serverOutputBudget_;
    std::unordered_map<
        EventLoop*,
        std::shared_ptr<TcpOutputMemoryBudget>>
        loopOutputBudgets_;
    mutable std::mutex outputMemoryBudgetsMutex_;
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
    std::unordered_map<std::string, DeadlineToken> authenticationDeadlines_;
    std::unique_ptr<DeadlineQueue> authenticationDeadlineQueue_;
    TimerId authenticationDeadlineDriver_;
    TimerId authenticationDeadlineContinuation_;
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
    mutable std::mutex stopLifecycleMutex_;
    std::shared_ptr<EventLoopLifecycleSource> stopLifecycleSource_;
    std::atomic<bool> immediateStopRequested_{false};
    std::uint64_t nextStopGeneration_{1};
    std::vector<std::shared_ptr<WorkerStopParticipant>>
        workerStopParticipants_;
    std::shared_ptr<AggregateStopState> aggregateStopState_;
    mutable std::mutex gracefulStopMutex_;
    std::shared_ptr<GracefulStopState> gracefulStopState_;
};

}  // namespace gamenet::net
