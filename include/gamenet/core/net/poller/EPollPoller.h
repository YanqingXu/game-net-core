#pragma once

// EPollPoller 是 Poller 的 Linux epoll 后端实现。
// 它负责把 Channel 注册关系映射到内核 epoll，并回填活跃事件。

#include "gamenet/core/net/Poller.h"

#ifndef _WIN32
namespace gamenet::net {

namespace detail {
class EpollReadinessPort;
}

class EPollPoller : public Poller {
public:
    explicit EPollPoller(EventLoop* loop);
    ~EPollPoller() override;

    gamenet::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    bool wakeup() override;

private:
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;
    std::unique_ptr<detail::EpollReadinessPort> readinessPort_;
};

}  // namespace gamenet::net

#endif
