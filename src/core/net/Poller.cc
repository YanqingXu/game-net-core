#include "gamenet/core/net/Poller.h"

#include "gamenet/core/net/Channel.h"

namespace gamenet::net {

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {
}

Poller::~Poller() = default;

void Poller::preserveSocketAssociation(SocketFd sockfd) {
    (void)sockfd;
}

void Poller::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    (void)operation;
    (void)lifetime;
}

bool Poller::wakeup() {
    return false;
}

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

}  // namespace gamenet::net
