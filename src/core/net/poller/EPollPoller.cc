#include "gamenet/core/net/poller/EPollPoller.h"

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"

#include "gamenet/core/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

namespace gamenet::net {

namespace {

[[noreturn]] void epollDie(const char* what) {
    LOG_SYSFATAL << what << ": " << std::strerror(errno);
    __builtin_unreachable();
}

uint32_t toEpollEvents(uint32_t events) noexcept {
    uint32_t epollEvents = 0;
    if (events & Channel::kReadEvent) {
        epollEvents |= EPOLLIN | EPOLLPRI;
    }
    if (events & Channel::kWriteEvent) {
        epollEvents |= EPOLLOUT;
    }
    return epollEvents;
}

uint32_t fromEpollEvents(uint32_t events) noexcept {
    uint32_t channelEvents = 0;
    if (events & (EPOLLIN | EPOLLPRI)) {
        channelEvents |= Channel::kReadEvent;
    }
    if (events & EPOLLOUT) {
        channelEvents |= Channel::kWriteEvent;
    }
    if (events & EPOLLERR) {
        channelEvents |= Channel::kErrorEvent;
    }
    if (events & (EPOLLHUP | EPOLLRDHUP)) {
        channelEvents |= Channel::kCloseEvent;
    }
    return channelEvents;
}

}  // namespace

EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop), epollfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        epollDie("epoll_create1");
    }
}

EPollPoller::~EPollPoller() {
    ::close(epollfd_);
}

gamenet::base::Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    const int numEvents =
        ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
    const auto now = gamenet::base::now();
    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        if (static_cast<std::size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents < 0 && errno != EINTR) {
        epollDie("epoll_wait");
    }
    return now;
}

void EPollPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const SocketFd fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            const auto [it, inserted] = channels_.emplace(fd, channel);
            if (!inserted) {
                throw std::logic_error(
                    it->second == channel
                        ? "EPollPoller new Channel is already registered"
                        : "EPollPoller fd belongs to a different Channel");
            }
        } else {
            const auto it = channels_.find(fd);
            if (it == channels_.end() || it->second != channel) {
                throw std::logic_error(
                    "EPollPoller deleted Channel registration identity mismatch");
            }
        }

        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
        return;
    }

    const auto existing = channels_.find(fd);
    if (existing == channels_.end() || existing->second != channel) {
        throw std::logic_error(
            "EPollPoller update Channel registration identity mismatch");
    }

    if (channel->isNoneEvent()) {
        update(EPOLL_CTL_DEL, channel);
        channel->setIndex(kDeleted);
    } else {
        update(EPOLL_CTL_MOD, channel);
    }
}

void EPollPoller::removeChannel(Channel* channel) {
    const SocketFd fd = channel->fd();
    const auto it = channels_.find(fd);
    if (it == channels_.end() || it->second != channel) {
        throw std::logic_error(
            "EPollPoller remove Channel registration identity mismatch");
    }

    if (channel->index() == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }
    channels_.erase(it);
    channel->setIndex(kNew);
}

void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        auto* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->setRevents(fromEpollEvents(events_[i].events));
        activeChannels->push_back(channel);
    }
}

void EPollPoller::update(int operation, Channel* channel) {
    epoll_event event{};
    event.events = toEpollEvents(channel->events());
    event.data.ptr = channel;
    if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
        epollDie("epoll_ctl");
    }
}

}  // namespace gamenet::net
