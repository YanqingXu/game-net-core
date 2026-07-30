#include "gamenet/core/net/poller/IocpPoller.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/platform/IocpOperation.h"
#include "../detail/NetworkMemoryRetentionTracker.h"

#include <algorithm>
#include <array>
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

constexpr ULONG_PTR kStatusCancelled = 0xC0000120UL;

DWORD completionError(
    IocpOperation& operation,
    bool channelRegistered) noexcept {
    const ULONG_PTR status = operation.overlapped.Internal;
    if (status == 0) {
        return 0;
    }
    if (status == kStatusCancelled) {
        return ERROR_OPERATION_ABORTED;
    }

    if (channelRegistered && operation.channel != nullptr) {
        DWORD transferred = 0;
        DWORD flags = 0;
        if (::WSAGetOverlappedResult(
                operation.channel->fd(),
                &operation.overlapped,
                &transferred,
                FALSE,
                &flags) != FALSE) {
            return 0;
        }
        return static_cast<DWORD>(::WSAGetLastError());
    }

    // A retained completion whose Channel observer was revoked is terminal
    // storage cleanup only. Preserve an explicit failure instead of treating
    // an untranslatable native status as a successful operation.
    return ERROR_GEN_FAILURE;
}

#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
std::atomic<bool> failNextAssociationPreserve{false};
std::atomic<bool> failNextReplacementRegistration{false};
std::atomic<SocketFd> lastAssociationFaultSocket{kInvalidSocket};
using WakeupResetHook = void (*)() noexcept;
std::atomic<WakeupResetHook> beforeWakeupResetHook{nullptr};
std::atomic<WakeupResetHook> afterWakeupResetHook{nullptr};
std::atomic<std::uint64_t> wakeupPacketsPosted{0};
std::atomic<std::uint64_t> wakeupPacketsConsumed{0};
#endif

}  // namespace

IocpPoller::IocpPoller(EventLoop* loop)
    : Poller(loop),
      iocp_(createCompletionPortOrDie()),
      completionBatchSize_(
          static_cast<ULONG>(
              loop->options_.maxIocpCompletionsPerPoll)) {
    detail::retainNetworkFixedStorage(
        detail::NetworkFixedStorageCategory::IocpCompletionBatch,
        sizeof(completionEntries_) +
            sizeof(deferredEntries_) +
            sizeof(publishedChannels_));
}

IocpPoller::~IocpPoller() {
    detail::releaseNetworkFixedStorage(
        detail::NetworkFixedStorageCategory::IocpCompletionBatch,
        sizeof(completionEntries_) +
            sizeof(deferredEntries_) +
            sizeof(publishedChannels_));
    if (iocp_ != nullptr) {
        ::CloseHandle(iocp_);
    }
}

gamenet::base::Timestamp IocpPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    lastCompletionPacketsDrained_ = 0;
    lastDeferredCompletionCount_ = 0;
    lastCompletionBudgetExhausted_ = false;
    ULONG removed = 0;
    BOOL ok = TRUE;
    const bool processingDeferredEntries = deferredEntryCount_ != 0;
    if (processingDeferredEntries) {
        removed = deferredEntryCount_;
        std::copy_n(
            deferredEntries_.begin(),
            removed,
            completionEntries_.begin());
        deferredEntryCount_ = 0;
    } else {
        ok = ::GetQueuedCompletionStatusEx(
            iocp_,
            completionEntries_.data(),
            completionBatchSize_,
            &removed,
            toTimeout(timeoutMs),
            FALSE);
    }

    const auto now = gamenet::base::now();
    if (!ok) {
        const DWORD error = ::GetLastError();
        if (error != WAIT_TIMEOUT) {
            iocpDie("GetQueuedCompletionStatusEx");
        }
        return now;
    }
    lastCompletionPacketsDrained_ = removed;
    lastCompletionBudgetExhausted_ =
        removed == completionBatchSize_;

    std::size_t activeCount = 0;

    for (ULONG index = 0; index < removed; ++index) {
        const auto& entry = completionEntries_[index];
        if (entry.lpCompletionKey == kWakeupCompletionKey) {
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            if (const auto hook =
                    beforeWakeupResetHook.load(std::memory_order_acquire);
                hook != nullptr) {
                hook();
            }
#endif
            wakeupPending_.store(false, std::memory_order_release);
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            wakeupPacketsConsumed.fetch_add(1, std::memory_order_relaxed);
            if (const auto hook =
                    afterWakeupResetHook.load(std::memory_order_acquire);
                hook != nullptr) {
                hook();
            }
#endif
            continue;
        }

        auto* operation =
            reinterpret_cast<IocpOperation*>(entry.lpOverlapped);
        if (operation == nullptr) {
            continue;
        }

        Channel* channel = operation->channel;
        const auto registered =
            channel == nullptr
            ? channels_.end()
            : channels_.find(channel->fd());
        const bool channelRegistered =
            registered != channels_.end() &&
            registered->second == channel &&
            channel->index() == kAdded;

        if (!processingDeferredEntries) {
            operation->bytesTransferred =
                entry.dwNumberOfBytesTransferred;
            operation->error =
                completionError(*operation, channelRegistered);
        }

        const bool channelAlreadyPublished =
            channelRegistered &&
            std::find(
                publishedChannels_.begin(),
                publishedChannels_.begin() +
                    static_cast<std::ptrdiff_t>(activeCount),
                channel) !=
                publishedChannels_.begin() +
                    static_cast<std::ptrdiff_t>(activeCount);
        if (channelAlreadyPublished &&
            operation->kind != IocpOperationKind::Accept) {
            if (deferredEntryCount_ >= deferredEntries_.size()) {
                iocpDie("IOCP deferred completion batch overflow");
            }
            deferredEntries_[deferredEntryCount_++] = entry;
            continue;
        }

        operation->completionObserved = true;
        if (operation->shutdownObligation) {
            operation->shutdownObligation = false;
            if (outstandingOperationCount_ == 0) {
                iocpDie("IOCP completion obligation underflow");
            }
            --outstandingOperationCount_;
        }
        std::shared_ptr<void> completionLifetime;
        const auto retained = retainedOperations_.find(operation);
        if (retained != retainedOperations_.end()) {
            completionLifetime = std::move(retained->second);
            retainedOperations_.erase(retained);
        }

        if (!channelRegistered) {
            continue;
        }

        if (operation->kind == IocpOperationKind::Accept) {
            // A listen Channel can have multiple independent AcceptEx slots.
            // Coalesce their exact identities into one allocation-free
            // owner-loop callback instead of forcing one loop turn per slot.
            channel->appendIocpAcceptCompletionOperation(operation);
        } else {
            channel->setIocpCompletionOperation(operation);
        }
        if (channelAlreadyPublished) {
            continue;
        }
        channel->setRevents(completionEvents(*operation));
        publishedChannels_[activeCount++] = channel;
        activeChannels->push_back(channel);
    }
    lastDeferredCompletionCount_ = deferredEntryCount_;
    return now;
}

void IocpPoller::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    if (operation != nullptr && lifetime) {
        retainedOperations_[operation] = std::move(lifetime);
    }
}

void IocpPoller::trackCompletionOperation(void* operation) {
    if (operation != nullptr) {
        auto* completion = static_cast<IocpOperation*>(operation);
        if (!completion->shutdownObligation) {
            completion->shutdownObligation = true;
            ++outstandingOperationCount_;
        }
    }
}

bool IocpPoller::hasPendingCompletionOperations() const noexcept {
    // retainedOperations_ is a storage lease and may include a live operation
    // whose subsystem intentionally outlives loop(). Only operations
    // explicitly committed to quiescing (for example, CancelIoEx during
    // TcpConnection close) are shutdown completion obligations.
    return outstandingOperationCount_ != 0;
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
    bool expected = false;
    if (!wakeupPending_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }

    if (::PostQueuedCompletionStatus(iocp_, 0, kWakeupCompletionKey, nullptr) == FALSE) {
        iocpDie("PostQueuedCompletionStatus(wakeup)");
    }
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
    wakeupPacketsPosted.fetch_add(1, std::memory_order_relaxed);
#endif
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

void resetIocpWakeupObservationsForTesting() noexcept {
    beforeWakeupResetHook.store(nullptr, std::memory_order_release);
    afterWakeupResetHook.store(nullptr, std::memory_order_release);
    wakeupPacketsPosted.store(0, std::memory_order_release);
    wakeupPacketsConsumed.store(0, std::memory_order_release);
}

void setIocpWakeupResetHooksForTesting(
    void (*beforeReset)() noexcept,
    void (*afterReset)() noexcept) noexcept {
    beforeWakeupResetHook.store(beforeReset, std::memory_order_release);
    afterWakeupResetHook.store(afterReset, std::memory_order_release);
}

std::uint64_t iocpWakeupPacketsPostedForTesting() noexcept {
    return wakeupPacketsPosted.load(std::memory_order_acquire);
}

std::uint64_t iocpWakeupPacketsConsumedForTesting() noexcept {
    return wakeupPacketsConsumed.load(std::memory_order_acquire);
}

}  // namespace detail
#endif

}  // namespace gamenet::net

#endif
