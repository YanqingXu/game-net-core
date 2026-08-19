#include "gamenet/core/net/poller/IocpPoller.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/platform/IocpOperation.h"
#include "../detail/CompletionPort.h"
#include "../detail/IocpOperationState.h"
#include "../detail/NetworkMemoryRetentionTracker.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
#include <atomic>
#endif
#include <new>
#include <stdexcept>

namespace gamenet::net::detail {

struct IocpCompletionState {
    std::array<OVERLAPPED_ENTRY, IocpPoller::kCompletionBatchSize>
        nativeEntries{};
    std::array<CompletionNotice, IocpPoller::kCompletionBatchSize>
        notices{};
    std::array<std::shared_ptr<void>, IocpPoller::kCompletionBatchSize>
        noticeLifetimes{};
    std::array<Channel*, IocpPoller::kCompletionBatchSize>
        publishedChannels{};
    ULONG noticeCount{0};
    CompletionWaitProgress progress{};
};

}  // namespace gamenet::net::detail

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

detail::CompletionOperationKind completionKind(
    IocpOperationKind kind) noexcept {
    switch (kind) {
    case IocpOperationKind::Accept:
        return detail::CompletionOperationKind::Accept;
    case IocpOperationKind::Connect:
        return detail::CompletionOperationKind::Connect;
    case IocpOperationKind::Read:
        return detail::CompletionOperationKind::Read;
    case IocpOperationKind::Write:
        return detail::CompletionOperationKind::Write;
    }
    std::terminate();
}

uint32_t completionEvents(
    detail::CompletionOperationKind kind) noexcept {
    switch (kind) {
    case detail::CompletionOperationKind::Accept:
    case detail::CompletionOperationKind::Read:
        return Channel::kReadEvent;
    case detail::CompletionOperationKind::Connect:
    case detail::CompletionOperationKind::Write:
        return Channel::kWriteEvent;
    }
    return Channel::kErrorEvent;
}

detail::CompletionTerminalStatus completionStatus(
    DWORD error) noexcept {
    if (error == 0) {
        return detail::CompletionTerminalStatus::Succeeded;
    }
    if (error == ERROR_OPERATION_ABORTED) {
        return detail::CompletionTerminalStatus::Cancelled;
    }
    return detail::CompletionTerminalStatus::Failed;
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
    : IocpPoller(
          loop,
          loop->options_.maxIocpCompletionsPerPoll) {}

IocpPoller::IocpPoller(
    EventLoop* loop,
    std::size_t completionBatchSize)
    : Poller(loop),
      completionState_(
          std::make_unique<detail::IocpCompletionState>()),
      iocp_(createCompletionPortOrDie()),
      completionBatchSize_(static_cast<ULONG>(completionBatchSize)) {
    if (completionBatchSize == 0 ||
        completionBatchSize > kCompletionBatchSize) {
        ::CloseHandle(iocp_);
        iocp_ = nullptr;
        throw std::invalid_argument(
            "IOCP completion batch size must be in [1, 64]");
    }
    detail::retainNetworkFixedStorage(
        detail::NetworkFixedStorageCategory::IocpCompletionBatch,
        sizeof(*completionState_));
}

IocpPoller::~IocpPoller() {
    detail::releaseNetworkFixedStorage(
        detail::NetworkFixedStorageCategory::IocpCompletionBatch,
        sizeof(*completionState_));
    if (iocp_ != nullptr) {
        ::CloseHandle(iocp_);
    }
}

gamenet::base::Timestamp IocpPoller::poll(
    int timeoutMs,
    ChannelList* activeChannels) {
    retireCompletionNoticeLeases();
    const auto batch = waitNativeCompletionNotices(timeoutMs);
    publishCompletionNotices(batch, activeChannels);
    lastDeferredCompletionCount_ = 0;
    return batch.observedAt;
}

detail::CompletionWaitResult IocpPoller::waitNativeCompletionNotices(
    int timeoutMs) {
    auto& state = *completionState_;
    retireCompletionNoticeLeases();
    state.noticeCount = 0;
    state.progress = {};
    lastCompletionPacketsDrained_ = 0;
    lastDeferredCompletionCount_ = 0;
    lastCompletionBudgetExhausted_ = false;
    ULONG removed = 0;
    const BOOL ok = ::GetQueuedCompletionStatusEx(
        iocp_,
        state.nativeEntries.data(),
        completionBatchSize_,
        &removed,
        toTimeout(timeoutMs),
        FALSE);

    const auto observedAt = gamenet::base::now();
    if (!ok) {
        const DWORD error = ::GetLastError();
        if (error != WAIT_TIMEOUT) {
            iocpDie("GetQueuedCompletionStatusEx");
        }
        return {
            .observedAt = observedAt,
            .notices = {},
            .progress = state.progress,
        };
    }
    state.progress.nativePackets = removed;
    state.progress.budgetExhausted =
        removed == completionBatchSize_;
    lastCompletionPacketsDrained_ = removed;
    lastCompletionBudgetExhausted_ = state.progress.budgetExhausted;

    for (ULONG index = 0; index < removed; ++index) {
        const auto& entry = state.nativeEntries[index];
        if (entry.lpCompletionKey == kWakeupCompletionKey) {
#ifdef GAMENET_INTERNAL_IOCP_TEST_HOOKS
            if (const auto hook =
                    beforeWakeupResetHook.load(std::memory_order_acquire);
                hook != nullptr) {
                hook();
            }
#endif
            wakeupPending_.store(false, std::memory_order_release);
            ++state.progress.wakeupPackets;
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
        if (operation == nullptr ||
            !operation->submissionAccepted ||
            operation->generation == 0 ||
            operation->terminalGeneration == operation->generation) {
            ++state.progress.invalidPackets;
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

        operation->bytesTransferred =
            entry.dwNumberOfBytesTransferred;
        operation->error =
            completionError(*operation, channelRegistered);
        const detail::CompletionOperationIdentity identity{
            .operation = operation,
            .generation = operation->generation,
        };
        if (!detail::retireIocpOperationSubmission(*operation)) {
            ++state.progress.invalidPackets;
            continue;
        }
        if (operation->terminalObserver != nullptr) {
            operation->terminalObserver(
                operation->terminalContext,
                operation->kind);
        }
        if (operation->shutdownObligation) {
            operation->shutdownObligation = false;
            if (outstandingOperationCount_ == 0) {
                iocpDie("IOCP completion obligation underflow");
            }
            --outstandingOperationCount_;
        }
        if (state.noticeCount >= completionBatchSize_) {
            iocpDie("IOCP typed completion batch overflow");
        }
        const auto retained = retainedOperations_.find(operation);
        if (retained != retainedOperations_.end()) {
            state.noticeLifetimes[state.noticeCount] =
                std::move(retained->second);
            retainedOperations_.erase(retained);
        }
        state.notices[state.noticeCount++] = detail::CompletionNotice{
            .identity = identity,
            .kind = completionKind(operation->kind),
            .observer = channelRegistered ? channel : nullptr,
            .bytesTransferred = operation->bytesTransferred,
            .nativeError = operation->error,
            .status = completionStatus(operation->error),
        };
    }
    state.progress.deliveredNotices = state.noticeCount;
    return {
        .observedAt = observedAt,
        .notices = std::span<const detail::CompletionNotice>(
            state.notices.data(),
            state.noticeCount),
        .progress = state.progress,
    };
}

void IocpPoller::publishCompletionNotices(
    const detail::CompletionWaitResult& batch,
    ChannelList* activeChannels) {
    auto& state = *completionState_;
    std::size_t activeCount = 0;
    for (std::size_t noticeIndex = 0;
         noticeIndex < batch.notices.size();
         ++noticeIndex) {
        const auto& notice = batch.notices[noticeIndex];
        Channel* channel = notice.observer;
        if (channel == nullptr) {
            continue;
        }
        const auto registered = channels_.find(channel->fd());
        if (registered == channels_.end() ||
            registered->second != channel ||
            channel->index() != kAdded) {
            continue;
        }
        auto* operation =
            static_cast<IocpOperation*>(notice.identity.operation);
        const bool channelAlreadyPublished =
            std::find(
                state.publishedChannels.begin(),
                state.publishedChannels.begin() +
                    static_cast<std::ptrdiff_t>(activeCount),
                channel) !=
                state.publishedChannels.begin() +
                    static_cast<std::ptrdiff_t>(activeCount);
        if (notice.kind == detail::CompletionOperationKind::Accept) {
            // A listen Channel can have multiple independent AcceptEx slots.
            // Coalesce their exact identities into one allocation-free
            // owner-loop callback instead of forcing one loop turn per slot.
            channel->appendIocpAcceptCompletionOperation(operation);
        } else {
            channel->setIocpCompletionOperation(operation);
        }
        if (channelAlreadyPublished) {
            channel->revents_ |= completionEvents(notice.kind);
            continue;
        }
        channel->setRevents(completionEvents(notice.kind));
        state.publishedChannels[activeCount++] = channel;
        activeChannels->push_back(channel);
    }
}

void IocpPoller::retireCompletionNoticeLeases() noexcept {
    for (auto& lifetime : completionState_->noticeLifetimes) {
        lifetime.reset();
    }
}

void IocpPoller::retainCompletionOperation(void* operation, std::shared_ptr<void> lifetime) {
    (void)commitNativeCompletionSubmission(
        operation,
        std::move(lifetime));
}

void IocpPoller::trackCompletionOperation(void* operation) {
    (void)commitNativeCompletionCancellation(operation);
}

bool IocpPoller::commitNativeCompletionSubmission(
    void* operation,
    std::shared_ptr<void> lifetime) {
    if (operation == nullptr || !lifetime ||
        retainedOperations_.contains(operation)) {
        return false;
    }
    const auto [retained, inserted] = retainedOperations_.emplace(
        operation,
        std::move(lifetime));
    if (!inserted) {
        return false;
    }
    auto* completion = static_cast<IocpOperation*>(operation);
    if (!detail::commitIocpOperationSubmission(*completion)) {
        retainedOperations_.erase(retained);
        return false;
    }
    return true;
}

bool IocpPoller::commitNativeCompletionCancellation(
    void* operation) noexcept {
    if (operation == nullptr) {
        return false;
    }
    auto* completion = static_cast<IocpOperation*>(operation);
    if (!completion->submissionAccepted ||
        completion->completionObserved ||
        completion->terminalGeneration == completion->generation) {
        return false;
    }
    if (!completion->shutdownObligation) {
        completion->shutdownObligation = true;
        ++outstandingOperationCount_;
    }
    return true;
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
