#pragma once

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/SocketTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gamenet::net {

class Channel;
class EventLoop;
class Poller;

namespace detail {

enum class IoEngineCapability : std::uint8_t {
    None = 0,
    Readiness = 1U << 0U,
    Completion = 1U << 1U,
    BackendWakeup = 1U << 2U,
};

constexpr IoEngineCapability operator|(
    IoEngineCapability left,
    IoEngineCapability right) noexcept {
    return static_cast<IoEngineCapability>(
        static_cast<std::uint8_t>(left) |
        static_cast<std::uint8_t>(right));
}

constexpr bool hasCapability(
    IoEngineCapability capabilities,
    IoEngineCapability capability) noexcept {
    return (static_cast<std::uint8_t>(capabilities) &
            static_cast<std::uint8_t>(capability)) != 0;
}

enum class IoEnginePhase : std::uint8_t {
    Running,
    Quiescing,
    // Physical backend shutdown occurs in EventLoop destruction, after
    // owner-thread teardown may unregister attached subsystems.
    Shutdown,
};

// Admission applies only to brand-new external work. Work already accepted by
// EventLoop before Quiescing may still perform owner-thread registration and
// cancellation while final drain converges.
enum class IoEngineAdmissionResult : std::uint8_t {
    Accepted,
    RejectedQuiescing,
    RejectedShutdown,
};

enum class IoEngineOperationResult : std::uint8_t {
    Accepted,
    RejectedInvalid,
    RejectedNotRegistered,
    RejectedConflict,
    RejectedUnsupported,
    RejectedShutdown,
};

constexpr bool accepted(IoEngineOperationResult result) noexcept {
    return result == IoEngineOperationResult::Accepted;
}

// Compatibility mapping for backend capacity. EventLoop scheduling budgets
// remain in EventLoopOptions and are intentionally absent from this type.
struct IoEngineOptions {
    std::size_t maxReadinessNoticesPerWait{64};
    std::size_t maxCompletionNoticesPerWait{64};
};

// IOE-R1 intentionally carries the existing readiness-shaped delivery buffer.
// IOE-C1 will add typed completion notices instead of translating them into
// this vector.
class IoNoticeBatch {
public:
    explicit IoNoticeBatch(std::vector<Channel*>& readiness) noexcept
        : readiness_(&readiness) {}

private:
    std::vector<Channel*>* readiness_;

    friend class PollerIoEngineAdapter;
};

struct IoCompletionProgress {
    std::size_t drained{0};
    std::size_t deferred{0};
    bool budgetExhausted{false};
};

struct IoWaitProgress {
    std::size_t deliveredNotices{0};
    std::size_t staleNotices{0};
    std::size_t wakeupNotices{0};
    bool budgetExhausted{false};
};

// Source-private owner-loop seam. All methods except wakeup() are owner-thread
// only. wakeup() is the one deliberate cross-thread interrupt capability.
class IoEngine {
public:
    virtual ~IoEngine() = default;

    virtual IoEngineCapability capabilities() const noexcept = 0;
    virtual IoEngineOptions options() const noexcept = 0;
    virtual IoEnginePhase phase() const noexcept = 0;
    virtual IoEngineAdmissionResult admission() const noexcept = 0;
    virtual gamenet::base::Timestamp wait(
        int timeoutMs,
        IoNoticeBatch& notices) = 0;
    virtual IoEngineOperationResult registerOrUpdateReadiness(
        Channel* channel) = 0;
    virtual IoEngineOperationResult cancelReadiness(Channel* channel) = 0;
    virtual bool hasReadiness(Channel* channel) const = 0;
    virtual bool wakeup() = 0;

    virtual IoEngineOperationResult commitSocketAssociationPreservation(
        SocketFd sockfd) = 0;
    virtual IoEngineOperationResult commitSocketAssociationForget(
        SocketFd sockfd) noexcept = 0;
    // Called only after the platform accepted a completion submission. The
    // optional lease keeps operation storage alive; no synchronous submit
    // failure may call this method.
    virtual IoEngineOperationResult commitCompletionSubmission(
        void* operation,
        std::shared_ptr<void> lifetime) = 0;
    // Commits a cancellation/final-drain obligation. Cancellation remains a
    // request; the obligation retires only when the terminal packet is read.
    virtual IoEngineOperationResult commitCompletionCancellation(
        void* operation) = 0;

    virtual void beginQuiesce() = 0;
    virtual bool quiescent() const noexcept = 0;
    virtual void markShutdown() = 0;
    virtual IoCompletionProgress completionProgress() const noexcept = 0;
    virtual IoWaitProgress waitProgress() const noexcept = 0;

};

// IOE-R1 keeps EventLoop's byte-stable private storage declaration as
// unique_ptr<Poller>. The created object implements both interfaces; production
// EventLoop code immediately recovers and uses only the Engine seam.
std::unique_ptr<Poller> makePollerIoEngineAdapter(
    EventLoop* loop,
    IoEngineOptions options);
IoEngine& ioEngineFromPoller(Poller& poller) noexcept;
const IoEngine& ioEngineFromPoller(const Poller& poller) noexcept;

}  // namespace detail
}  // namespace gamenet::net
