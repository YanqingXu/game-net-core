#include "IoEngine.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Poller.h"
#ifdef _WIN32
#include "IocpPollerAccess.h"
#include "gamenet/core/net/poller/IocpPoller.h"
#else
#include "gamenet/core/net/poller/EPollPoller.h"
#endif

#include <exception>
#include <stdexcept>
#include <utility>

namespace gamenet::net::detail {

#ifdef _WIN32
using NativePoller = IocpPoller;
#else
using NativePoller = EPollPoller;
#endif

// Multiple inheritance is deliberate and source-private. Poller preserves the
// byte-stable EventLoop storage declaration; IoEngine is the only production
// call surface used by EventLoop.cc during IOE-R1.
class PollerIoEngineAdapter final : public NativePoller, public IoEngine {
public:
    PollerIoEngineAdapter(EventLoop* loop, IoEngineOptions options)
#ifdef _WIN32
        : NativePoller(loop, options.maxCompletionNoticesPerWait),
#else
        : NativePoller(loop),
#endif
          ownerLoop_(loop),
          options_(options) {
        if (ownerLoop_ == nullptr) {
            throw std::invalid_argument(
                "PollerIoEngineAdapter requires an owning EventLoop");
        }
        if (options_.maxCompletionNoticesPerWait == 0 ||
            options_.maxCompletionNoticesPerWait > 64) {
            throw std::invalid_argument(
                "I/O Engine completion capacity must be in [1, 64]");
        }
    }

    IoEngineCapability capabilities() const noexcept override {
#ifdef _WIN32
        return IoEngineCapability::Completion |
            IoEngineCapability::BackendWakeup;
#else
        return IoEngineCapability::Readiness;
#endif
    }

    IoEngineOptions options() const noexcept override {
        return options_;
    }

    IoEnginePhase phase() const noexcept override {
        return phase_;
    }

    IoEngineAdmissionResult admission() const noexcept override {
        switch (phase_) {
            case IoEnginePhase::Running:
                return IoEngineAdmissionResult::Accepted;
            case IoEnginePhase::Quiescing:
                return IoEngineAdmissionResult::RejectedQuiescing;
            case IoEnginePhase::Shutdown:
                return IoEngineAdmissionResult::RejectedShutdown;
        }
        std::terminate();
    }

    gamenet::base::Timestamp wait(
        int timeoutMs,
        IoNoticeBatch& notices) override {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Shutdown) {
            throw std::logic_error("I/O Engine wait after shutdown");
        }
        return NativePoller::poll(timeoutMs, notices.readiness_);
    }

    IoEngineOperationResult registerOrUpdateReadiness(
        Channel* channel) override {
        assertOwnerThread();
        if (channel == nullptr) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown &&
            !NativePoller::hasChannel(channel)) {
            return IoEngineOperationResult::RejectedShutdown;
        }
        try {
            NativePoller::updateChannel(channel);
        } catch (const std::logic_error&) {
            return IoEngineOperationResult::RejectedConflict;
        }
        return IoEngineOperationResult::Accepted;
    }

    IoEngineOperationResult cancelReadiness(Channel* channel) override {
        assertOwnerThread();
        if (channel == nullptr) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
        if (!NativePoller::hasChannel(channel)) {
            return IoEngineOperationResult::RejectedNotRegistered;
        }
        NativePoller::removeChannel(channel);
        return IoEngineOperationResult::Accepted;
    }

    bool hasReadiness(Channel* channel) const override {
        assertOwnerThread();
        return channel != nullptr && NativePoller::hasChannel(channel);
    }

    bool wakeup() override {
        // The backend wakeup primitive is the one cross-thread Engine method.
        // EventLoop's admission locks guarantee its storage outlives the call.
#ifdef _WIN32
        return NativePoller::wakeup();
#else
        return false;
#endif
    }

    IoEngineOperationResult commitSocketAssociationPreservation(
        SocketFd sockfd) override {
        assertOwnerThread();
        if (sockfd == kInvalidSocket) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        NativePoller::preserveSocketAssociation(sockfd);
        return IoEngineOperationResult::Accepted;
#else
        (void)sockfd;
        return IoEngineOperationResult::RejectedUnsupported;
#endif
    }

    IoEngineOperationResult commitSocketAssociationForget(
        SocketFd sockfd) noexcept override {
        if (!ownerLoop_->isInLoopThread()) {
            std::terminate();
        }
        if (sockfd == kInvalidSocket) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        NativePoller::forgetSocketAssociation(sockfd);
        return IoEngineOperationResult::Accepted;
#else
        (void)sockfd;
        return IoEngineOperationResult::RejectedUnsupported;
#endif
    }

    IoEngineOperationResult commitCompletionSubmission(
        void* operation,
        std::shared_ptr<void> lifetime) override {
        assertOwnerThread();
        if (operation == nullptr || !lifetime) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        NativePoller::retainCompletionOperation(
            operation,
            std::move(lifetime));
        return IoEngineOperationResult::Accepted;
#else
        (void)operation;
        (void)lifetime;
        return IoEngineOperationResult::RejectedUnsupported;
#endif
    }

    IoEngineOperationResult commitCompletionCancellation(
        void* operation) override {
        assertOwnerThread();
        if (operation == nullptr) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        NativePoller::trackCompletionOperation(operation);
        return IoEngineOperationResult::Accepted;
#else
        (void)operation;
        return IoEngineOperationResult::RejectedUnsupported;
#endif
    }

    void beginQuiesce() override {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Running) {
            phase_ = IoEnginePhase::Quiescing;
        }
    }

    bool quiescent() const noexcept override {
#ifdef _WIN32
        return !NativePoller::hasPendingCompletionOperations();
#else
        return true;
#endif
    }

    void markShutdown() override {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Shutdown) {
            return;
        }
        // IOE-R1 preserves Poller's destructor behavior. Some legacy owners
        // cancel backend work while unwinding after loop() has returned; the
        // Poller closes its backend before releasing retained storage. IOE-C1
        // will move that teardown into an explicit terminal-notice drain.
        phase_ = IoEnginePhase::Shutdown;
    }

    IoCompletionProgress completionProgress() const noexcept override {
#ifdef _WIN32
        return IocpPollerAccess::completionProgress(*this);
#else
        return {};
#endif
    }

private:
    void assertOwnerThread() const {
        ownerLoop_->assertInLoopThread();
    }

    EventLoop* ownerLoop_;
    IoEngineOptions options_;
    IoEnginePhase phase_{IoEnginePhase::Running};
};

std::unique_ptr<Poller> makePollerIoEngineAdapter(
    EventLoop* loop,
    IoEngineOptions options) {
    return std::make_unique<PollerIoEngineAdapter>(loop, options);
}

IoEngine& ioEngineFromPoller(Poller& poller) noexcept {
    return static_cast<PollerIoEngineAdapter&>(poller);
}

const IoEngine& ioEngineFromPoller(const Poller& poller) noexcept {
    return static_cast<const PollerIoEngineAdapter&>(poller);
}

}  // namespace gamenet::net::detail
