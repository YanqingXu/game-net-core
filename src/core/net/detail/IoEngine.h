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

// Source-private owner-loop seam. All methods except wakeup() are owner-thread
// only. wakeup() is the one deliberate cross-thread interrupt capability.
class IoEngine {
public:
    virtual ~IoEngine() = default;

    virtual IoEngineCapability capabilities() const noexcept = 0;
    virtual IoEnginePhase phase() const noexcept = 0;
    virtual gamenet::base::Timestamp wait(
        int timeoutMs,
        IoNoticeBatch& notices) = 0;
    virtual void updateReadiness(Channel* channel) = 0;
    virtual void removeReadiness(Channel* channel) = 0;
    virtual bool hasReadiness(Channel* channel) const = 0;
    virtual bool wakeup() = 0;

    virtual void preserveSocketAssociation(SocketFd sockfd) = 0;
    virtual void forgetSocketAssociation(SocketFd sockfd) noexcept = 0;
    virtual void retainCompletionOperation(
        void* operation,
        std::shared_ptr<void> lifetime) = 0;
    virtual void trackCompletionOperation(void* operation) = 0;

    virtual void beginQuiesce() = 0;
    virtual bool quiescent() const noexcept = 0;
    virtual void markShutdown() = 0;
    virtual IoCompletionProgress completionProgress() const noexcept = 0;

};

// IOE-R1 keeps EventLoop's byte-stable private storage declaration as
// unique_ptr<Poller>. The created object implements both interfaces; production
// EventLoop code immediately recovers and uses only the Engine seam.
std::unique_ptr<Poller> makePollerIoEngineAdapter(EventLoop* loop);
IoEngine& ioEngineFromPoller(Poller& poller) noexcept;
const IoEngine& ioEngineFromPoller(const Poller& poller) noexcept;

}  // namespace detail
}  // namespace gamenet::net
