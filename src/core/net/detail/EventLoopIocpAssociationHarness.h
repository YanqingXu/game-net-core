#pragma once

// Source-private deterministic inspection seam for the Windows association
// handoff contract. It is compiled only by repository tests and is not part
// of the installed scheduling API.

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#include "gamenet/core/net/platform/IocpOperation.h"
#endif

#include <cstddef>

namespace gamenet::net::detail {

#ifdef _WIN32
void injectNextIocpAssociationPreserveFailureForTesting() noexcept;
void injectNextIocpReplacementRegistrationFailureForTesting() noexcept;
SocketFd lastIocpAssociationFaultSocketForTesting() noexcept;
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

    static std::size_t completionBatchSize() noexcept {
#ifdef _WIN32
        return IocpPoller::kCompletionBatchSize;
#else
        return 0;
#endif
    }

    static void trackCompletion(
        EventLoop& loop,
        IocpOperation* operation) {
#ifdef _WIN32
        loop.trackCompletionOperation(operation);
#else
        (void)loop;
        (void)operation;
#endif
    }

    static bool postCompletion(
        EventLoop& loop,
        IocpOperation* operation,
        DWORD bytesTransferred) noexcept {
#ifdef _WIN32
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
#else
        (void)loop;
        (void)operation;
        (void)bytesTransferred;
        return false;
#endif
    }

    static std::size_t pollAndDispatch(EventLoop& loop) {
#ifdef _WIN32
        loop.activeChannels_.clear();
        loop.pollReturnTime_ =
            loop.poller_->poll(0, &loop.activeChannels_);
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
        return loop.poller_->hasPendingCompletionOperations();
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
};

}  // namespace gamenet::net::detail
