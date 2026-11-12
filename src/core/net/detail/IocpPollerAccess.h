// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source-private production access to the IOCP adapter state that has not yet
// moved behind the native Completion Engine. Repository test harnesses remain
// separate from this seam.

#include "IoEngine.h"
#include "CompletionPort.h"
#include "IocpOperationState.h"
#include <stdexcept>

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#endif

#include <utility>

namespace gamenet::net::detail {

class IocpPollerAccess final {
public:
    IocpPollerAccess() = delete;

#ifdef _WIN32
    // Reserve operation storage before a datagram native call. Commit below
    // consumes this prepared entry only after the kernel accepted submission.
    static void prepareDatagramSubmission(
        IocpPoller& poller,
        IocpOperation& op,
        std::shared_ptr<void> lifetime) {
        if (!prepareIocpOperationSubmission(op))
            throw std::logic_error("pending UDP operation reuse");
        try {
            if (!poller.retainedOperations_.emplace(&op, std::move(lifetime))
                     .second)
                throw std::logic_error("UDP operation already retained");
        } catch (...) {
            (void)rejectIocpOperationSubmission(op);
            throw;
        }
    }
    static void rejectDatagramSubmission(
        IocpPoller& poller,
        IocpOperation& op) noexcept {
        (void)rejectIocpOperationSubmission(op);
        poller.retainedOperations_.erase(&op);
    }

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

    static CompletionWaitResult waitCompletionEngine(
        IocpPoller& poller,
        int timeoutMs) {
        return poller.waitNativeCompletionNotices(timeoutMs);
    }

    static std::size_t pendingCompletionNoticeCount(
        const IocpPoller& poller) noexcept {
        return poller.pendingDirectCompletionNoticeCount();
    }

    static bool takeNextCompletionNotice(
        IocpPoller& poller,
        CompletionNotice* notice) noexcept {
        return poller.takeNextDirectCompletionNotice(notice);
    }

    static bool completionObserverCurrent(
        const IocpPoller& poller,
        const CompletionNotice& notice) noexcept {
        return poller.completionObserverCurrent(notice);
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
