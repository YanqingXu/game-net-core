#include "gamenet/core/net/Channel.h"

#include "gamenet/core/net/EventLoop.h"

#include "gamenet/core/base/Logger.h"

#include <stdexcept>
#include <utility>

namespace gamenet::net {

Channel::Channel(EventLoop* loop, SocketFd fd)
    : loop_(loop),
      fd_(fd),
      events_(kNoneEvent),
      revents_(0),
      index_(-1),
      eventHandling_(false),
      addedToLoop_(false),
      registrationGeneration_(0),
      activeBatchEpoch_(0),
      activeBatchIndex_(0),
      tied_(false) {
}

Channel::~Channel() {
    if (eventHandling_) {
        LOG_FATAL << "Channel destroyed while handling events";
    }
    if (addedToLoop_) {
        LOG_FATAL << "Channel destroyed without remove-before-destroy";
    }
}

void Channel::handleEvent(gamenet::base::Timestamp receiveTime) {
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (guard) {
            handleEventWithGuard(receiveTime);
        }
        return;
    }
    handleEventWithGuard(receiveTime);
}

void Channel::setReadCallback(ReadEventCallback cb) {
    readCallback_ = std::move(cb);
}

void Channel::setWriteCallback(EventCallback cb) {
    writeCallback_ = std::move(cb);
}

void Channel::setCloseCallback(EventCallback cb) {
    closeCallback_ = std::move(cb);
}

void Channel::setErrorCallback(EventCallback cb) {
    errorCallback_ = std::move(cb);
}

void Channel::tie(const std::shared_ptr<void>& object) {
    tie_ = object;
    tied_ = true;
}

SocketFd Channel::fd() const noexcept {
    return fd_;
}

uint32_t Channel::events() const noexcept {
    return events_;
}

void Channel::setRevents(uint32_t revents) noexcept {
    revents_ = revents;
}

bool Channel::isNoneEvent() const noexcept {
    return events_ == kNoneEvent;
}

bool Channel::isWriting() const noexcept {
    return (events_ & kWriteEvent) != 0;
}

bool Channel::isReading() const noexcept {
    return (events_ & kReadEvent) != 0;
}

void Channel::enableReading() {
    if ((events_ & kReadEvent) != 0) {
        return;
    }
    events_ |= kReadEvent;
    update();
}

void Channel::disableReading() {
    if ((events_ & kReadEvent) == 0) {
        return;
    }
    events_ &= ~kReadEvent;
    update();
}

void Channel::enableWriting() {
    if ((events_ & kWriteEvent) != 0) {
        return;
    }
    events_ |= kWriteEvent;
    update();
}

void Channel::disableWriting() {
    if ((events_ & kWriteEvent) == 0) {
        return;
    }
    events_ &= ~kWriteEvent;
    update();
}

void Channel::disableAll() {
    if (events_ == kNoneEvent) {
        return;
    }
    events_ = kNoneEvent;
    update();
}

void Channel::remove() {
    if (!addedToLoop_) {
        throw std::logic_error("Channel::remove requires an active registration");
    }
    if (!isNoneEvent()) {
        throw std::runtime_error("Channel::remove requires disableAll() first");
    }
    loop_->removeChannel(this);
}

int Channel::index() const noexcept {
    return index_;
}

void Channel::setIndex(int index) noexcept {
    index_ = index;
}

EventLoop* Channel::ownerLoop() noexcept {
    return loop_;
}

void Channel::update() {
    loop_->updateChannel(this);
}

void Channel::advanceRegistrationGeneration() noexcept {
    ++registrationGeneration_;
    if (registrationGeneration_ == 0) {
        ++registrationGeneration_;
    }
}

void Channel::handleEventWithGuard(gamenet::base::Timestamp receiveTime) {
    const std::uint64_t dispatchGeneration = registrationGeneration_;
    eventHandling_ = true;
    try {
        if ((revents_ & kCloseEvent) && !(revents_ & kReadEvent)) {
            if (closeCallback_) {
                closeCallback_();
            }
            if (registrationGeneration_ != dispatchGeneration) {
                eventHandling_ = false;
                return;
            }
        }
        if (revents_ & kErrorEvent) {
            if (errorCallback_) {
                errorCallback_();
            }
            if (registrationGeneration_ != dispatchGeneration) {
                eventHandling_ = false;
                return;
            }
        }
        if (revents_ & kReadEvent) {
            if (readCallback_) {
                readCallback_(receiveTime);
            }
            if (registrationGeneration_ != dispatchGeneration) {
                eventHandling_ = false;
                return;
            }
        }
        if (revents_ & kWriteEvent) {
            if (writeCallback_) {
                writeCallback_();
            }
        }
    } catch (...) {
        eventHandling_ = false;
        throw;
    }

    eventHandling_ = false;
}

}  // namespace gamenet::net
