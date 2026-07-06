#include "gamenet/core/net/Poller.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#else
#include "gamenet/core/net/poller/EPollPoller.h"
#endif

#include <memory>

namespace gamenet::net {

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
#ifdef _WIN32
    return std::make_unique<IocpPoller>(loop);
#else
    return std::make_unique<EPollPoller>(loop);
#endif
}

}  // namespace gamenet::net
