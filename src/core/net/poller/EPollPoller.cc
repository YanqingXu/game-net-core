#include "gamenet/core/net/poller/EPollPoller.h"

#include "../detail/EpollReadinessPort.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"

#include <stdexcept>
#include <string>

namespace gamenet::net {

namespace {

[[noreturn]] void rejectReadinessMutation(
    const char* operation,
    detail::ReadinessPortResult result) {
    throw std::logic_error(
        std::string("EPollPoller ") + operation +
        " rejected by readiness port: " +
        std::to_string(static_cast<unsigned int>(result)));
}

}  // namespace

EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop),
      readinessPort_(
          std::make_unique<detail::EpollReadinessPort>(loop)) {}

EPollPoller::~EPollPoller() = default;

gamenet::base::Timestamp EPollPoller::poll(
    int timeoutMs,
    ChannelList* activeChannels) {
    const auto batch = readinessPort_->wait(timeoutMs);
    for (const auto& notice : batch.notices) {
        const auto existing = channels_.find(notice.identity.source);
        if (existing == channels_.end() ||
            existing->second != notice.target ||
            !readinessPort_->isCurrent(notice.identity, notice.target)) {
            continue;
        }
        notice.target->setRevents(notice.events);
        activeChannels->push_back(notice.target);
    }
    return batch.observedAt;
}

void EPollPoller::updateChannel(Channel* channel) {
    if (channel == nullptr) {
        throw std::invalid_argument(
            "EPollPoller update requires a Channel");
    }
    const auto existing = channels_.find(channel->fd());
    if (existing != channels_.end() && existing->second != channel) {
        throw std::logic_error(
            "EPollPoller fd belongs to a different Channel");
    }

    const bool newCompatibilityRegistration = existing == channels_.end();
    if (newCompatibilityRegistration) {
        channels_.emplace(channel->fd(), channel);
    }
    detail::ReadinessRegistrationResult registered;
    try {
        registered = readinessPort_->registerOrUpdate({
            .source = channel->fd(),
            .target = channel,
            .interests = channel->events(),
        });
    } catch (...) {
        if (newCompatibilityRegistration) {
            channels_.erase(channel->fd());
        }
        throw;
    }
    if (registered.result != detail::ReadinessPortResult::Accepted) {
        if (newCompatibilityRegistration) {
            channels_.erase(channel->fd());
        }
        rejectReadinessMutation("update", registered.result);
    }
    channel->setIndex(
        channel->isNoneEvent() ? kDeleted : kAdded);
}

void EPollPoller::removeChannel(Channel* channel) {
    if (channel == nullptr) {
        throw std::invalid_argument(
            "EPollPoller remove requires a Channel");
    }
    const auto existing = channels_.find(channel->fd());
    if (existing == channels_.end() || existing->second != channel) {
        throw std::logic_error(
            "EPollPoller remove Channel registration identity mismatch");
    }
    const auto result = readinessPort_->cancel(channel);
    if (result != detail::ReadinessPortResult::Accepted) {
        rejectReadinessMutation("remove", result);
    }
    channels_.erase(existing);
    channel->setIndex(kNew);
}

bool EPollPoller::wakeup() {
    return readinessPort_->wakeup();
}

}  // namespace gamenet::net
