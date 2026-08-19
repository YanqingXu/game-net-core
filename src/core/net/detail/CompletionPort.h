#pragma once

// Source-private Completion vocabulary. A notice represents one native
// operation result; it is never coalesced by socket or translated into a
// readiness mask at this boundary.

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/SocketTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace gamenet::net {

class Channel;

namespace detail {

enum class CompletionOperationKind : std::uint8_t {
    Accept,
    Connect,
    Read,
    Write,
};

enum class CompletionTerminalStatus : std::uint8_t {
    Succeeded,
    Failed,
    Cancelled,
};

struct CompletionOperationIdentity {
    void* operation{nullptr};
    std::uint64_t generation{0};

    bool valid() const noexcept {
        return operation != nullptr && generation != 0;
    }

    bool operator==(const CompletionOperationIdentity&) const = default;
};

using CompletionConsumer =
    void (*)(
        void* context,
        gamenet::base::Timestamp observedAt,
        bool observerCurrent);

struct CompletionNotice {
    CompletionOperationIdentity identity{};
    CompletionOperationKind kind{CompletionOperationKind::Read};
    Channel* observer{nullptr};
    SocketFd observerSource{kInvalidSocket};
    std::uint64_t observerGeneration{0};
    std::size_t bytesTransferred{0};
    std::uint32_t nativeError{0};
    CompletionTerminalStatus status{CompletionTerminalStatus::Failed};
    void* consumerContext{nullptr};
    CompletionConsumer consumer{nullptr};

    bool terminal() const noexcept {
        return true;
    }
};

struct CompletionWaitProgress {
    std::size_t nativePackets{0};
    std::size_t deliveredNotices{0};
    std::size_t invalidPackets{0};
    std::size_t wakeupPackets{0};
    bool budgetExhausted{false};
};

struct CompletionWaitResult {
    gamenet::base::Timestamp observedAt{};
    std::span<const CompletionNotice> notices{};
    CompletionWaitProgress progress{};
};

}  // namespace detail
}  // namespace gamenet::net
