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

    // loop must be non-null and outlive this Poller, including instances
    // returned by newDefaultPoller().
    explicit Poller(EventLoop* loop);
    // Poller does not own Channels or EventLoop. Mutation and destruction are
    // owner-loop-only; concrete backend obligation hooks are private.
    virtual ~Poller();

    virtual gamenet::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    bool hasChannel(Channel* channel) const;
    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<SocketFd, Channel*>;
    ChannelMap channels_;

private:
    // Backend lifecycle hooks are available only to EventLoop. In particular,
    // raw completion identities are not part of the stable application API.
    virtual void preserveSocketAssociation(SocketFd sockfd);
    virtual void retainCompletionOperation(
        void* operation,
        std::shared_ptr<void> lifetime);
    virtual void trackCompletionOperation(void* operation);
    virtual bool hasPendingCompletionOperations() const noexcept;
    virtual bool wakeup();

    EventLoop* ownerLoop_;

    friend class EventLoop;
};

}  // namespace gamenet::net
