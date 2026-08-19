#pragma once

// Source-private deterministic inspection seam for Windows IOCP association,
// completion-drain, and wakeup contracts. It is compiled only by repository
// tests and is not part of the installed scheduling API.

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"
#include "IoEngine.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#include "gamenet/core/net/platform/IocpOperation.h"
#include "IocpOperationState.h"
#include "IocpPollerAccess.h"
#endif

#include <cstddef>
#include <stdexcept>

namespace gamenet::net::detail {

#ifdef _WIN32
void injectNextIocpAssociationPreserveFailureForTesting() noexcept;
void injectNextIocpReplacementRegistrationFailureForTesting() noexcept;
SocketFd lastIocpAssociationFaultSocketForTesting() noexcept;
void resetIocpWakeupObservationsForTesting() noexcept;
void setIocpWakeupResetHooksForTesting(
    void (*beforeReset)() noexcept,
    void (*afterReset)() noexcept) noexcept;
std::uint64_t iocpWakeupPacketsPostedForTesting() noexcept;
std::uint64_t iocpWakeupPacketsConsumedForTesting() noexcept;
#endif

class EventLoopIocpAssociationHarness final {
public:
    EventLoopIocpAssociationHarness() = delete;

    static void failNextPreserve() noexcept {
#ifdef _WIN32
        injectNextIocpAssociationPreserveFailureForTesting();
#endif
    }

    static void failNextReplacementRegistration() noexcept {
#ifdef _WIN32
        injectNextIocpReplacementRegistrationFailureForTesting();
#endif
    }

    static SocketFd lastFaultSocket() noexcept {
#ifdef _WIN32
        return lastIocpAssociationFaultSocketForTesting();
#else
        return kInvalidSocket;
#endif
    }

    static bool tracks(
        EventLoop& loop,
        SocketFd socket) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller != nullptr &&
               poller->associatedFds_.contains(socket);
#else
        (void)loop;
        (void)socket;
        return false;
#endif
    }

    static void preserveSocketAssociation(
        EventLoop& loop,
        SocketFd socket) {
        loop.preserveSocketAssociation(socket);
    }

    static std::size_t completionBatchSize() noexcept {
#ifdef _WIN32
        return IocpPoller::kCompletionBatchSize;
#else
        return 0;
#endif
    }

    static std::size_t configuredCompletionBatchSize(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller == nullptr ? 0 : poller->completionBatchSize_;
#else
        (void)loop;
        return 0;
#endif
    }

    static std::size_t lastCompletionPacketsDrained(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller == nullptr
            ? 0
            : poller->lastCompletionPacketsDrained_;
#else
        (void)loop;
        return 0;
#endif
    }

    static bool lastCompletionBudgetExhausted(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller != nullptr &&
               poller->lastCompletionBudgetExhausted_;
#else
        (void)loop;
        return false;
#endif
    }

    static IoCompletionProgress completionProgress(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        const auto* poller =
            dynamic_cast<const IocpPoller*>(loop.poller_.get());
        if (poller != nullptr) {
            return {
                poller->lastCompletionPacketsDrained_,
                poller->lastDeferredCompletionCount_,
                poller->lastCompletionBudgetExhausted_,
            };
        }
#else
        (void)loop;
#endif
        return {};
    }

#ifdef _WIN32
    static bool beginSyntheticCompletionSubmission(
        IocpOperation* operation) noexcept {
        if (operation == nullptr ||
            !prepareIocpOperationSubmission(*operation)) {
            return false;
        }
        if (commitIocpOperationSubmission(*operation)) {
            return true;
        }
        (void)rejectIocpOperationSubmission(*operation);
        return false;
    }

    static bool prepareRejectedCompletionSubmission(
        IocpOperation* operation) noexcept {
        return operation != nullptr &&
               prepareIocpOperationSubmission(*operation) &&
               rejectIocpOperationSubmission(*operation);
    }

    static CompletionWaitResult waitNativeCompletions(
        EventLoop& loop,
        int timeoutMs) {
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller == nullptr
            ? CompletionWaitResult{}
            : IocpPollerAccess::waitNativeCompletions(
                  *poller,
                  timeoutMs);
    }

    static void retireNativeCompletions(EventLoop& loop) noexcept {
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        if (poller != nullptr) {
            IocpPollerAccess::retireCompletionNotices(*poller);
        }
    }

    static void trackCompletion(
        EventLoop& loop,
        IocpOperation* operation) {
        if (operation != nullptr && !operation->submissionAccepted &&
            !beginSyntheticCompletionSubmission(operation)) {
            throw std::logic_error(
                "test IOCP completion submission conflict");
        }
        loop.trackCompletionOperation(operation);
    }

    static bool postCompletion(
        EventLoop& loop,
        IocpOperation* operation,
        DWORD bytesTransferred) noexcept {
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        if (poller == nullptr || operation == nullptr) {
            return false;
        }
        return ::PostQueuedCompletionStatus(
                   poller->iocp_,
                   bytesTransferred,
                   operation->channel == nullptr
                       ? 0
                       : static_cast<ULONG_PTR>(
                             operation->channel->fd()),
                   &operation->overlapped) != FALSE;
    }
#endif

    static std::size_t waitAndDispatch(EventLoop& loop) {
#ifdef _WIN32
        loop.activeChannels_.clear();
        loop.activeChannelCursor_ = 0;
        auto& ioEngine = ioEngineFromPoller(*loop.poller_);
        IoNoticeBatch notices(loop.activeChannels_);
        loop.pollReturnTime_ =
            ioEngine.wait(0, notices);
        loop.emitIocpCompletionMetric();
        const std::size_t activeCount =
            loop.activeChannels_.size();
        loop.dispatchActiveChannels();
        return activeCount;
#else
        (void)loop;
        return 0;
#endif
    }

    static bool hasPendingCompletionOperations(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        return loop.hasPendingCompletionOperations();
#else
        (void)loop;
        return false;
#endif
    }

    static std::size_t outstandingCompletionCount(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller == nullptr
            ? 0
            : poller->outstandingOperationCount_;
#else
        (void)loop;
        return 0;
#endif
    }

    static std::size_t retainedCompletionCount(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller == nullptr
            ? 0
            : poller->retainedOperations_.size();
#else
        (void)loop;
        return 0;
#endif
    }

    static void resetWakeupObservations() noexcept {
#ifdef _WIN32
        resetIocpWakeupObservationsForTesting();
#endif
    }

    static void setWakeupResetHooks(
        void (*beforeReset)() noexcept,
        void (*afterReset)() noexcept) noexcept {
#ifdef _WIN32
        setIocpWakeupResetHooksForTesting(beforeReset, afterReset);
#else
        (void)beforeReset;
        (void)afterReset;
#endif
    }

    static std::uint64_t logicalWakeupCount(
        EventLoop& loop) noexcept {
        return loop.wakeupCount_.load(std::memory_order_acquire);
    }

    static std::uint64_t physicalWakeupPacketsPosted() noexcept {
#ifdef _WIN32
        return iocpWakeupPacketsPostedForTesting();
#else
        return 0;
#endif
    }

    static std::uint64_t physicalWakeupPacketsConsumed() noexcept {
#ifdef _WIN32
        return iocpWakeupPacketsConsumedForTesting();
#else
        return 0;
#endif
    }

    static bool wakeupPending(
        EventLoop& loop) noexcept {
#ifdef _WIN32
        auto* poller =
            dynamic_cast<IocpPoller*>(loop.poller_.get());
        return poller != nullptr &&
               poller->wakeupPending_.load(std::memory_order_acquire);
#else
        (void)loop;
        return false;
#endif
    }
};

}  // namespace gamenet::net::detail
