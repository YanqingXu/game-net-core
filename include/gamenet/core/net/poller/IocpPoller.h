#pragma once

// Windows IOCP backend for EventLoop.
// IocpPoller owns the completion port, associates loop-owned socket handles,
// translates completion packets into Channel events, and posts wakeup packets
// through the same backend abstraction. TCP read/write completions arrive via
// loop-owned IocpOperation metadata owned by the connection transport; Poller
// observes that metadata but does not own Channel or TcpConnection.

#include "gamenet/core/net/Poller.h"

#ifdef _WIN32

#include <array>
#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace gamenet::net {

namespace detail {
class EventLoopIocpAssociationHarness;
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

    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;
    static constexpr ULONG_PTR kWakeupCompletionKey = static_cast<ULONG_PTR>(-1);
    static constexpr ULONG kCompletionBatchSize = 64;

    void associateChannel(Channel* channel);

    HANDLE iocp_;
    ULONG completionBatchSize_;
    std::atomic<bool> wakeupPending_{false};
    std::array<OVERLAPPED_ENTRY, kCompletionBatchSize> completionEntries_{};
    std::array<OVERLAPPED_ENTRY, kCompletionBatchSize> deferredEntries_{};
    std::array<Channel*, kCompletionBatchSize> publishedChannels_{};
    ULONG deferredEntryCount_{0};
    std::unordered_set<SocketFd> associatedFds_;
    std::size_t outstandingOperationCount_{0};
    std::unordered_map<void*, std::shared_ptr<void>> retainedOperations_;
    ULONG lastCompletionPacketsDrained_{0};
    ULONG lastDeferredCompletionCount_{0};
    bool lastCompletionBudgetExhausted_{false};
};

}  // namespace gamenet::net

#endif
