#pragma once

// Source-private production access to the IOCP adapter state that has not yet
// moved behind the native Completion Engine. Repository test harnesses remain
// separate from this seam.

#include "IoEngine.h"
#include "CompletionPort.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#endif

#include <utility>

namespace gamenet::net::detail {

class IocpPollerAccess final {
public:
    IocpPollerAccess() = delete;

#ifdef _WIN32
    static IoCompletionProgress completionProgress(
        const IocpPoller& poller) noexcept {
        return {
            poller.lastCompletionPacketsDrained_,
            poller.lastDeferredCompletionCount_,
            poller.lastCompletionBudgetExhausted_,
        };
    }

    static CompletionWaitResult waitNativeCompletions(
        IocpPoller& poller,
        int timeoutMs) {
        return poller.waitNativeCompletionNotices(timeoutMs);
    }

    static void retireCompletionNotices(
        IocpPoller& poller) noexcept {
        poller.retireCompletionNoticeLeases();
    }

    static bool commitCompletionSubmission(
        IocpPoller& poller,
        void* operation,
        std::shared_ptr<void> lifetime) {
        return poller.commitNativeCompletionSubmission(
            operation,
            std::move(lifetime));
    }

    static bool commitCompletionCancellation(
        IocpPoller& poller,
        void* operation) noexcept {
        return poller.commitNativeCompletionCancellation(operation);
    }
#endif
};

}  // namespace gamenet::net::detail
