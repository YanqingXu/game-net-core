#include "gamenet/core/net/poller/IocpPoller.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cstdlib>

namespace gamenet::net {

namespace {

[[noreturn]] void iocpDie(const char* what) {
    LOG_SYSFATAL << what << ": " << static_cast<unsigned long>(::GetLastError());
    std::abort();
}

HANDLE createCompletionPortOrDie() {
    sockets::ensureInitialized();
    HANDLE iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (iocp == nullptr) {
        iocpDie("CreateIoCompletionPort");
    }
    return iocp;
}

uint32_t completionEvents(Channel* channel, bool succeeded) noexcept {
    if (!succeeded) {
        return Channel::kErrorEvent | Channel::kCloseEvent;
    }
    const uint32_t events = channel->events();
    return events == Channel::kNoneEvent ? Channel::kReadEvent : events;
}

DWORD toTimeout(int timeoutMs) noexcept {
    return timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
}

}  // namespace

IocpPoller::IocpPoller(EventLoop* loop)
    : Poller(loop),
      iocp_(createCompletionPortOrDie()) {}

IocpPoller::~IocpPoller() {
    if (iocp_ != nullptr) {
        ::CloseHandle(iocp_);
    }
}

gamenet::base::Timestamp IocpPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    OVERLAPPED* overlapped = nullptr;

    const BOOL ok = ::GetQueuedCompletionStatus(
        iocp_,
        &bytesTransferred,
        &completionKey,
        &overlapped,
        toTimeout(timeoutMs));
    (void)bytesTransferred;

    const auto now = gamenet::base::now();
    if (!ok && overlapped == nullptr) {
        const DWORD error = ::GetLastError();
        if (error != WAIT_TIMEOUT) {
            iocpDie("GetQueuedCompletionStatus");
        }
        return now;
    }

    const auto fd = static_cast<SocketFd>(completionKey);
    const auto it = channels_.find(fd);
    if (it != channels_.end() && it->second->index() == kAdded) {
        it->second->setRevents(completionEvents(it->second, ok != FALSE));
        activeChannels->push_back(it->second);
    }
    return now;
}

void IocpPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const SocketFd fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            channels_[fd] = channel;
            associateChannel(channel);
        }
        channel->setIndex(channel->isNoneEvent() ? kDeleted : kAdded);
        return;
    }

    if (channel->isNoneEvent()) {
        channel->setIndex(kDeleted);
    } else {
        channel->setIndex(kAdded);
    }
}

void IocpPoller::removeChannel(Channel* channel) {
    channels_.erase(channel->fd());
    channel->setIndex(kNew);
}

void IocpPoller::associateChannel(Channel* channel) {
    const auto associated = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(channel->fd()),
        iocp_,
        static_cast<ULONG_PTR>(channel->fd()),
        0);
    if (associated == nullptr) {
        iocpDie("CreateIoCompletionPort(socket)");
    }
}

}  // namespace gamenet::net

#endif
