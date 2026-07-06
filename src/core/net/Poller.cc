#include "gamenet/core/net/Poller.h"

#include "gamenet/core/net/Channel.h"

namespace gamenet::net {

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {
}

Poller::~Poller() = default;

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

}  // namespace gamenet::net
