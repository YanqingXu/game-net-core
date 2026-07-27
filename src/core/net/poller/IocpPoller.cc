#include "gamenet/core/net/poller/IocpPoller.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include <cstdlib>
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include <atomic>
#endif
#include <new>
#include <stdexcept>

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

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
std::atomic<bool> failNextAssociationPreserve{false};
std::atomic<bool> failNextReplacementRegistration{false};
std::atomic<SocketFd> lastAssociationFaultSocket{kInvalidSocket};
#endif

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
    outstandingOperations_.erase(operation);
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

void IocpPoller::trackCompletionOperation(void* operation) {
    if (operation != nullptr) {
        outstandingOperations_.insert(operation);
    }
}

bool IocpPoller::hasPendingCompletionOperations() const noexcept {
    // retainedOperations_ is a storage lease and may include a live operation
    // whose subsystem intentionally outlives loop(). Only operations
    // explicitly committed to quiescing (for example, CancelIoEx during
    // TcpConnection close) are shutdown completion obligations.
    return !outstandingOperations_.empty();
}

void IocpPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const SocketFd fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        const auto existing = channels_.find(fd);
        const bool alreadyAssociated = associatedFds_.contains(fd);
        if (index == kNew) {
            if (existing != channels_.end()) {
                throw std::logic_error(
                    existing->second == channel
                        ? "IocpPoller new Channel is already registered"
                        : "IocpPoller fd belongs to a different Channel");
            }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            if (alreadyAssociated &&
                failNextReplacementRegistration.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                lastAssociationFaultSocket.store(
                    fd,
                    std::memory_order_release);
                throw std::bad_alloc{};
            }
#endif
            channels_.emplace(fd, channel);
        } else if (existing == channels_.end() ||
                   existing->second != channel) {
            throw std::logic_error(
                "IocpPoller deleted Channel registration identity mismatch");
        }

        if (!alreadyAssociated) {
            try {
                associateChannel(channel);
                associatedFds_.insert(fd);
            } catch (...) {
                if (index == kNew) {
                    channels_.erase(fd);
                }
                throw;
            }
        }
        channel->setIndex(channel->isNoneEvent() ? kDeleted : kAdded);
        return;
    }

    const auto existing = channels_.find(fd);
    if (existing == channels_.end() || existing->second != channel) {
        throw std::logic_error(
            "IocpPoller update Channel registration identity mismatch");
    }

    if (channel->isNoneEvent()) {
        channel->setIndex(kDeleted);
    } else {
        channel->setIndex(kAdded);
    }
}

void IocpPoller::removeChannel(Channel* channel) {
    const SocketFd fd = channel->fd();
    const auto it = channels_.find(fd);
    if (it == channels_.end() || it->second != channel) {
        throw std::logic_error(
            "IocpPoller remove Channel registration identity mismatch");
    }
    channels_.erase(it);
    associatedFds_.erase(fd);
    channel->setIndex(kNew);
}

void IocpPoller::preserveSocketAssociation(SocketFd sockfd) {
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    if (failNextAssociationPreserve.exchange(
            false,
            std::memory_order_acq_rel)) {
        lastAssociationFaultSocket.store(
            sockfd,
            std::memory_order_release);
        throw std::bad_alloc{};
    }
#endif
    associatedFds_.insert(sockfd);
}

void IocpPoller::forgetSocketAssociation(SocketFd sockfd) noexcept {
    associatedFds_.erase(sockfd);
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

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
namespace detail {

void injectNextIocpAssociationPreserveFailureForTesting() noexcept {
    failNextAssociationPreserve.store(true, std::memory_order_release);
}

void injectNextIocpReplacementRegistrationFailureForTesting() noexcept {
    failNextReplacementRegistration.store(true, std::memory_order_release);
}

SocketFd lastIocpAssociationFaultSocketForTesting() noexcept {
    return lastAssociationFaultSocket.load(std::memory_order_acquire);
}

}  // namespace detail
#endif

}  // namespace gamenet::net

#endif
