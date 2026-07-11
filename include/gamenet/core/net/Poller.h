#pragma once

// Poller 抽象 I/O 多路复用后端，只维护注册关系与活跃 Channel 收集。
// 它不拥有 Channel，也不能脱离所属 EventLoop 的线程纪律独立工作。

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/base/noncopyable.h"
#include "gamenet/core/net/SocketTypes.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace gamenet::net {

class Channel;
class EventLoop;

class Poller : private gamenet::base::noncopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop);
    virtual ~Poller();

    virtual gamenet::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;
    // Preserve backend socket-handle association across fd ownership transfer.
    virtual void preserveSocketAssociation(SocketFd sockfd);
    // Keep backend operation storage alive until its completion is observed.
    virtual void retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime);
    virtual bool wakeup();

    bool hasChannel(Channel* channel) const;
    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<SocketFd, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* ownerLoop_;
};

}  // namespace gamenet::net
