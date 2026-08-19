#pragma once

// Source-private readiness vocabulary. A native port owns registration
// identity and waiting, but borrows Channel callback targets from EventLoop.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/SocketTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gamenet::net {

class Channel;

namespace detail {

enum class ReadinessPortResult : std::uint8_t {
    Accepted,
    RejectedInvalid,
    RejectedNotRegistered,
    RejectedConflict,
    RejectedShutdown,
};

enum class ReadinessTriggerMode : std::uint8_t {
    Level,
};

struct ReadinessPortOptions {
    std::size_t maxNoticesPerWait{64};
    ReadinessTriggerMode triggerMode{ReadinessTriggerMode::Level};
};

struct ReadinessRegistrationIdentity {
    SocketFd source{kInvalidSocket};
    std::uint64_t generation{0};

    bool valid() const noexcept {
        return source != kInvalidSocket && generation != 0;
    }

    bool operator==(const ReadinessRegistrationIdentity&) const = default;
};

struct ReadinessRegistrationRequest {
    SocketFd source{kInvalidSocket};
    Channel* target{nullptr};
    std::uint32_t interests{0};
};

struct ReadinessRegistrationResult {
    ReadinessPortResult result{ReadinessPortResult::RejectedInvalid};
    ReadinessRegistrationIdentity identity{};
};

struct ReadinessNotice {
    ReadinessRegistrationIdentity identity{};
    Channel* target{nullptr};
    std::uint32_t events{0};
};

struct ReadinessWaitProgress {
    std::size_t nativeNotices{0};
    std::size_t deliveredNotices{0};
    std::size_t staleNotices{0};
    std::size_t wakeupNotices{0};
    bool budgetExhausted{false};
};

struct ReadinessWaitResult {
    gamenet::base::Timestamp observedAt{};
    std::span<const ReadinessNotice> notices{};
    ReadinessWaitProgress progress{};
};

class ReadinessPort {
public:
    virtual ~ReadinessPort() = default;

    virtual ReadinessPortOptions options() const noexcept = 0;
    virtual ReadinessRegistrationResult registerOrUpdate(
        ReadinessRegistrationRequest request) = 0;
    virtual ReadinessPortResult cancel(Channel* target) = 0;
    virtual bool has(const Channel* target) const = 0;
    virtual std::optional<ReadinessRegistrationIdentity>
    registrationIdentity(const Channel* target) const = 0;
    virtual bool isCurrent(
        ReadinessRegistrationIdentity identity,
        const Channel* target) const = 0;
    virtual ReadinessWaitResult wait(int timeoutMs) = 0;

    // The only cross-thread method. It interrupts wait without granting
    // registration admission or carrying callback/business data.
    virtual bool wakeup() noexcept = 0;
};

}  // namespace detail
}  // namespace gamenet::net
