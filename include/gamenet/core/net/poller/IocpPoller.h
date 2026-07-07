#pragma once

// Windows IOCP backend for EventLoop.
// IocpPoller owns the completion port, associates loop-owned socket handles,
// translates completion packets into Channel events, and posts wakeup packets
// through the same backend abstraction. TCP read/write completions arrive via
// loop-owned IocpOperation metadata owned by the connection transport; Poller
// observes that metadata but does not own Channel or TcpConnection.

#include "gamenet/core/net/Poller.h"

#ifdef _WIN32

#include <unordered_set>

namespace gamenet::net {

class IocpPoller : public Poller {
public:
    explicit IocpPoller(EventLoop* loop);
    ~IocpPoller() override;

    gamenet::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    void preserveSocketAssociation(SocketFd sockfd) override;
    bool wakeup() override;

private:
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;
    static constexpr ULONG_PTR kWakeupCompletionKey = static_cast<ULONG_PTR>(-1);

    void associateChannel(Channel* channel);

    HANDLE iocp_;
    std::unordered_set<SocketFd> associatedFds_;
};

}  // namespace gamenet::net

#endif
