#pragma once

// Windows IOCP backend for EventLoop.
// IocpPoller owns the completion port, associates loop-owned socket handles,
// decodes completion packets into fixed typed terminal notices, temporarily
// compatibility-publishes surviving observers as Channel events, and posts
// wakeup packets through the same backend abstraction. TCP read/write
// operations are owned by the connection transport; Poller owns neither
// Channel nor TcpConnection and carries no raw deferred pointer across a
// callback boundary.

#include "gamenet/core/net/Poller.h"

#ifdef _WIN32

#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace gamenet::net {

namespace detail {
struct CompletionWaitResult;
class EventLoopIocpAssociationHarness;
struct IocpCompletionState;
class IocpPollerAccess;
}

class IocpPoller : public Poller {
public:
    explicit IocpPoller(EventLoop* loop);
    IocpPoller(EventLoop* loop, std::size_t completionBatchSize);
    ~IocpPoller() override;

    gamenet::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    void preserveSocketAssociation(SocketFd sockfd) override;
    void forgetSocketAssociation(SocketFd sockfd) noexcept;
    void retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) override;
    void trackCompletionOperation(void* operation) override;
    bool hasPendingCompletionOperations() const noexcept override;
    bool wakeup() override;

private:
    friend class EventLoop;
    friend class detail::EventLoopIocpAssociationHarness;
    friend class detail::IocpPollerAccess;
    friend struct detail::IocpCompletionState;

    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;
    static constexpr ULONG_PTR kWakeupCompletionKey = static_cast<ULONG_PTR>(-1);
    static constexpr ULONG kCompletionBatchSize = 64;

    void associateChannel(Channel* channel);
    detail::CompletionWaitResult waitNativeCompletionNotices(
        int timeoutMs);
    void publishCompletionNotices(
        const detail::CompletionWaitResult& batch,
        ChannelList* activeChannels);
    void retireCompletionNoticeLeases() noexcept;
    bool commitNativeCompletionSubmission(
        void* operation,
        std::shared_ptr<void> lifetime);
    bool commitNativeCompletionCancellation(void* operation) noexcept;

    std::unique_ptr<detail::IocpCompletionState> completionState_;
    HANDLE iocp_;
    ULONG completionBatchSize_;
    std::atomic<bool> wakeupPending_{false};
    std::unordered_set<SocketFd> associatedFds_;
    std::size_t outstandingOperationCount_{0};
    std::unordered_map<void*, std::shared_ptr<void>> retainedOperations_;
    ULONG lastCompletionPacketsDrained_{0};
    ULONG lastDeferredCompletionCount_{0};
    bool lastCompletionBudgetExhausted_{false};
};

}  // namespace gamenet::net

#endif
