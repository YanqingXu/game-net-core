#include "IoEngine.h"

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/Poller.h"
#ifdef _WIN32
#include "IocpPollerAccess.h"
#include "gamenet/core/net/poller/IocpPoller.h"
#else
#include "EpollReadinessPort.h"
#endif

#include <exception>
#include <stdexcept>
#include <utility>

namespace gamenet::net::detail {

#ifdef _WIN32
using NativePoller = IocpPoller;
#else
using NativePoller = Poller;

IoEngineOperationResult toIoEngineResult(
    ReadinessPortResult result) noexcept {
    switch (result) {
        case ReadinessPortResult::Accepted:
            return IoEngineOperationResult::Accepted;
        case ReadinessPortResult::RejectedInvalid:
            return IoEngineOperationResult::RejectedInvalid;
        case ReadinessPortResult::RejectedNotRegistered:
            return IoEngineOperationResult::RejectedNotRegistered;
        case ReadinessPortResult::RejectedConflict:
            return IoEngineOperationResult::RejectedConflict;
        case ReadinessPortResult::RejectedShutdown:
            return IoEngineOperationResult::RejectedShutdown;
    }
    std::terminate();
}
#endif

// The dual Poller/IoEngine object is deliberate and source-private. Poller
// preserves EventLoop's byte-stable storage declaration; Linux directly owns
// EpollReadinessPort and leaves Poller's legacy map empty, while Windows
// retains the IocpPoller compatibility base. The separately constructed
// EPollPoller compatibility shell continues to maintain that legacy map.
class PollerIoEngineAdapter final : public NativePoller, public IoEngine {
public:
    PollerIoEngineAdapter(EventLoop* loop, IoEngineOptions options)
#ifdef _WIN32
        : NativePoller(loop, options.maxCompletionNoticesPerWait),
#else
        : NativePoller(loop),
#endif
          ownerLoop_(loop),
          options_(options)
#ifndef _WIN32
          , readinessPort_(
                loop,
                ReadinessPortOptions{
                    .maxNoticesPerWait =
                        options.maxReadinessNoticesPerWait,
                })
#endif
    {
        if (ownerLoop_ == nullptr) {
            throw std::invalid_argument(
                "PollerIoEngineAdapter requires an owning EventLoop");
        }
        if (options_.maxCompletionNoticesPerWait == 0 ||
            options_.maxCompletionNoticesPerWait > 64) {
            throw std::invalid_argument(
                "I/O Engine completion capacity must be in [1, 64]");
        }
        if (options_.maxReadinessNoticesPerWait == 0 ||
            options_.maxReadinessNoticesPerWait > 4096) {
            throw std::invalid_argument(
                "I/O Engine readiness capacity must be in [1, 4096]");
        }
    }

    IoEngineCapability capabilities() const noexcept override {
#ifdef _WIN32
        return IoEngineCapability::Completion |
            IoEngineCapability::BackendWakeup;
#else
        return IoEngineCapability::Readiness |
            IoEngineCapability::BackendWakeup;
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
#ifdef _WIN32
        const auto batch = IocpPollerAccess::waitCompletionEngine(
            *this,
            timeoutMs,
            notices.readiness_);
        lastWaitProgress_ = {
            .deliveredNotices = batch.progress.deliveredNotices,
            .staleNotices = batch.progress.invalidPackets,
            .wakeupNotices = batch.progress.wakeupPackets,
            .budgetExhausted = batch.progress.budgetExhausted,
        };
        return batch.observedAt;
#else
        return waitForReadiness(timeoutMs, notices.readiness_);
#endif
    }

    void retireWaitBatch() noexcept override {
#ifdef _WIN32
        IocpPollerAccess::retireCompletionNotices(*this);
#endif
    }

    std::size_t pendingCompletionNoticeCount() const noexcept override {
#ifdef _WIN32
        return IocpPollerAccess::pendingCompletionNoticeCount(*this);
#else
        return 0;
#endif
    }

    bool takeNextCompletionNotice(
        CompletionNotice* notice) noexcept override {
#ifdef _WIN32
        return IocpPollerAccess::takeNextCompletionNotice(
            *this,
            notice);
#else
        (void)notice;
        return false;
#endif
    }

    bool completionObserverCurrent(
        const CompletionNotice& notice) const noexcept override {
#ifdef _WIN32
        return IocpPollerAccess::completionObserverCurrent(
            *this,
            notice);
#else
        (void)notice;
        return false;
#endif
    }

    IoEngineOperationResult registerOrUpdateReadiness(
        Channel* channel) override {
        assertOwnerThread();
        if (channel == nullptr) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown &&
            !hasReadiness(channel)) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        try {
            NativePoller::updateChannel(channel);
        } catch (const std::logic_error&) {
            return IoEngineOperationResult::RejectedConflict;
        }
        return IoEngineOperationResult::Accepted;
#else
        const auto registered = readinessPort_.registerOrUpdate({
            .source = channel->fd(),
            .target = channel,
            .interests = channel->events(),
        });
        if (registered.result != ReadinessPortResult::Accepted) {
            return toIoEngineResult(registered.result);
        }
        channel->setIndex(channel->isNoneEvent() ? kDeleted : kAdded);
        return IoEngineOperationResult::Accepted;
#endif
    }

    IoEngineOperationResult cancelReadiness(Channel* channel) override {
        assertOwnerThread();
        if (channel == nullptr) {
            return IoEngineOperationResult::RejectedInvalid;
        }
        if (phase_ == IoEnginePhase::Shutdown) {
            return IoEngineOperationResult::RejectedShutdown;
        }
#ifdef _WIN32
        if (!hasReadiness(channel)) {
            return IoEngineOperationResult::RejectedNotRegistered;
        }
        NativePoller::removeChannel(channel);
        return IoEngineOperationResult::Accepted;
#else
        const auto result = readinessPort_.cancel(channel);
        if (result != ReadinessPortResult::Accepted) {
            return toIoEngineResult(result);
        }
        channel->setIndex(kNew);
        return IoEngineOperationResult::Accepted;
#endif
    }

    bool hasReadiness(Channel* channel) const override {
        assertOwnerThread();
        if (channel == nullptr) {
            return false;
        }
#ifdef _WIN32
        return NativePoller::hasChannel(channel);
#else
        return readinessPort_.has(channel);
#endif
    }

    bool wakeup() override {
        // The backend wakeup primitive is the one cross-thread Engine method.
        // EventLoop's admission locks guarantee its storage outlives the call.
#ifdef _WIN32
        return NativePoller::wakeup();
#else
        return readinessPort_.wakeup();
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
        return IocpPollerAccess::commitCompletionSubmission(
                   *this,
                   operation,
                   std::move(lifetime))
            ? IoEngineOperationResult::Accepted
            : IoEngineOperationResult::RejectedConflict;
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
        return IocpPollerAccess::commitCompletionCancellation(
                   *this,
                   operation)
            ? IoEngineOperationResult::Accepted
            : IoEngineOperationResult::RejectedConflict;
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

    IoWaitProgress waitProgress() const noexcept override {
        return lastWaitProgress_;
    }

#ifndef _WIN32
    gamenet::base::Timestamp poll(
        int timeoutMs,
        ChannelList* activeChannels) override {
        return waitForReadiness(timeoutMs, activeChannels);
    }

    void updateChannel(Channel* channel) override {
        const auto result = registerOrUpdateReadiness(channel);
        if (!accepted(result)) {
            throw std::logic_error(
                "readiness Poller compatibility update rejected");
        }
    }

    void removeChannel(Channel* channel) override {
        const auto result = cancelReadiness(channel);
        if (!accepted(result)) {
            throw std::logic_error(
                "readiness Poller compatibility remove rejected");
        }
    }
#endif

private:
    void assertOwnerThread() const {
        ownerLoop_->assertInLoopThread();
    }

#ifndef _WIN32
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;

    gamenet::base::Timestamp waitForReadiness(
        int timeoutMs,
        ChannelList* activeChannels) {
        const auto batch = readinessPort_.wait(timeoutMs);
        lastWaitProgress_ = {
            .deliveredNotices = batch.progress.deliveredNotices,
            .staleNotices = batch.progress.staleNotices,
            .wakeupNotices = batch.progress.wakeupNotices,
            .budgetExhausted = batch.progress.budgetExhausted,
        };
        for (const auto& notice : batch.notices) {
            if (!readinessPort_.isCurrent(
                    notice.identity,
                    notice.target)) {
                continue;
            }
            notice.target->setRevents(notice.events);
            activeChannels->push_back(notice.target);
        }
        return batch.observedAt;
    }
#endif

    EventLoop* ownerLoop_;
    IoEngineOptions options_;
    IoWaitProgress lastWaitProgress_{};
#ifndef _WIN32
    EpollReadinessPort readinessPort_;
#endif
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
