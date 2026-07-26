#pragma once

// Source-private deterministic inspection seam for the Windows association
// handoff contract. It is compiled only by repository tests and is not part
// of the installed scheduling API.

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32
#include "gamenet/core/net/poller/IocpPoller.h"
#endif

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
};

}  // namespace gamenet::net::detail
