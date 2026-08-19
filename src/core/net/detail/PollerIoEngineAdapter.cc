#include "IoEngine.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Poller.h"
#ifdef _WIN32
#include "EventLoopIocpAssociationHarness.h"
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
    explicit PollerIoEngineAdapter(EventLoop* loop)
        : NativePoller(loop), ownerLoop_(loop) {
        if (ownerLoop_ == nullptr) {
            throw std::invalid_argument(
                "PollerIoEngineAdapter requires an owning EventLoop");
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

    IoEnginePhase phase() const noexcept override {
        return phase_;
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

    void updateReadiness(Channel* channel) override {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Shutdown &&
            !NativePoller::hasChannel(channel)) {
            throw std::logic_error("I/O Engine admission is sealed");
        }
        NativePoller::updateChannel(channel);
    }

    void removeReadiness(Channel* channel) override {
        assertOwnerThread();
        NativePoller::removeChannel(channel);
    }

    bool hasReadiness(Channel* channel) const override {
        assertOwnerThread();
        return NativePoller::hasChannel(channel);
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

    void preserveSocketAssociation(SocketFd sockfd) override {
        assertAvailableOwnerThread();
#ifdef _WIN32
        NativePoller::preserveSocketAssociation(sockfd);
#else
        (void)sockfd;
#endif
    }

    void forgetSocketAssociation(SocketFd sockfd) noexcept override {
        if (!ownerLoop_->isInLoopThread()) {
            std::terminate();
        }
#ifdef _WIN32
        NativePoller::forgetSocketAssociation(sockfd);
#else
        (void)sockfd;
#endif
    }

    void retainCompletionOperation(
        void* operation,
        std::shared_ptr<void> lifetime) override {
        assertAvailableOwnerThread();
#ifdef _WIN32
        NativePoller::retainCompletionOperation(
            operation,
            std::move(lifetime));
#else
        (void)operation;
        (void)lifetime;
#endif
    }

    void trackCompletionOperation(void* operation) override {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Shutdown) {
            throw std::logic_error(
                "I/O Engine completion tracking after shutdown");
        }
#ifdef _WIN32
        NativePoller::trackCompletionOperation(operation);
#else
        (void)operation;
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
        return EventLoopIocpAssociationHarness::completionProgress(
            *ownerLoop_);
#else
        return {};
#endif
    }

private:
    void assertOwnerThread() const {
        ownerLoop_->assertInLoopThread();
    }

    void assertAvailableOwnerThread() const {
        assertOwnerThread();
        if (phase_ == IoEnginePhase::Shutdown) {
            throw std::logic_error("I/O Engine admission is sealed");
        }
    }

    EventLoop* ownerLoop_;
    IoEnginePhase phase_{IoEnginePhase::Running};
};

std::unique_ptr<Poller> makePollerIoEngineAdapter(EventLoop* loop) {
    return std::make_unique<PollerIoEngineAdapter>(loop);
}

IoEngine& ioEngineFromPoller(Poller& poller) noexcept {
    return static_cast<PollerIoEngineAdapter&>(poller);
}

const IoEngine& ioEngineFromPoller(const Poller& poller) noexcept {
    return static_cast<const PollerIoEngineAdapter&>(poller);
}

}  // namespace gamenet::net::detail
