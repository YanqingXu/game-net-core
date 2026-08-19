#pragma once

// Source-private production access to the IOCP adapter state that has not yet
// moved behind the native Completion Engine. Repository test harnesses remain
// separate from this seam.

#include "IoEngine.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#endif

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
#endif
};

}  // namespace gamenet::net::detail
