#include "gamenet/core/net/poller/IocpPoller.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/platform/IocpOperation.h"

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

uint32_t completionEvents(const IocpOperation& operation) noexcept {
    switch (operation.kind) {
    case IocpOperationKind::Accept:
    case IocpOperationKind::Read:
        return Channel::kReadEvent;
    case IocpOperationKind::Connect:
    case IocpOperationKind::Write:
        return Channel::kWriteEvent;
    }
    return Channel::kErrorEvent;
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

    if (completionKey == kWakeupCompletionKey) {
        return now;
    }

    auto* operation = reinterpret_cast<IocpOperation*>(overlapped);
    if (operation == nullptr) {
        return now;
    }
    std::shared_ptr<void> completionLifetime;
    const auto retained = retainedOperations_.find(operation);
    if (retained != retainedOperations_.end()) {
        completionLifetime = std::move(retained->second);
        retainedOperations_.erase(retained);
    }
    if (operation->channel == nullptr) return now;

    operation->bytesTransferred = bytesTransferred;
    operation->error = ok ? 0 : ::GetLastError();

    Channel* channel = operation->channel;
    const auto it = channels_.find(channel->fd());
    if (it != channels_.end() && it->second == channel && channel->index() == kAdded) {
        channel->setRevents(completionEvents(*operation));
        activeChannels->push_back(channel);
    }
    return now;
}

void IocpPoller::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    if (operation != nullptr && lifetime) {
        retainedOperations_[operation] = std::move(lifetime);
    }
}

void IocpPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const SocketFd fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        const bool alreadyAssociated = associatedFds_.contains(fd);
        channels_[fd] = channel;
        if (!alreadyAssociated) {
            associateChannel(channel);
            associatedFds_.insert(fd);
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
    const SocketFd fd = channel->fd();
    channels_.erase(fd);
    associatedFds_.erase(fd);
    channel->setIndex(kNew);
}

void IocpPoller::preserveSocketAssociation(SocketFd sockfd) {
    associatedFds_.insert(sockfd);
}

bool IocpPoller::wakeup() {
    if (::PostQueuedCompletionStatus(iocp_, 0, kWakeupCompletionKey, nullptr) == FALSE) {
        iocpDie("PostQueuedCompletionStatus(wakeup)");
    }
    return true;
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
