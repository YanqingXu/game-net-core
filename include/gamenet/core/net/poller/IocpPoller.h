#pragma once

// IocpPoller 是 Windows IOCP 后端骨架。
// 它拥有 completion port 并把 socket 句柄关联到该端口；实际 TCP
// overlapped read/write completion state 由后续连接层迁移接入。

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
