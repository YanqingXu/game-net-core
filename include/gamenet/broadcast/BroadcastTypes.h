#pragma once

#include "gamenet/game_session/PlayerSession.h"
#include "gamenet/transport/TransportEndpoint.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gamenet::broadcast {

class BroadcastRouter;
class BroadcastDispatcher;

enum class BroadcastPriority : std::uint8_t {
    Low,
    Normal,
    High,
};

enum class BroadcastMetricEvent {
    Routed,
    Scheduled,
    Sent,
    Dropped,
};

enum class BroadcastReason {
    None,
    OfflineSession,
    DuplicateEndpoint,
    FanoutHardLimit,
    ByteHardLimit,
    LowPrioritySoftLimit,
    DispatchTaskByteLimit,
    EndpointClosed,
    EndpointOverloaded,
    OwnerUnavailable,
    OwnerShutdown,
    DispatchQueueFull,
    OwnerOutstandingTaskLimit,
    OwnerOutstandingByteLimit,
    GlobalOutstandingByteLimit,
    InvalidPlan,
    SendRejected,
    Count,
};

inline constexpr std::size_t kBroadcastReasonCount =
    static_cast<std::size_t>(BroadcastReason::Count);

constexpr std::size_t reasonIndex(BroadcastReason reason) noexcept {
    return static_cast<std::size_t>(reason);
}

struct BroadcastMetric {
    BroadcastMetricEvent event{BroadcastMetricEvent::Routed};
    BroadcastReason reason{BroadcastReason::None};
    gamenet::transport::TransportSessionId transportId{};
    std::size_t payloadBytes{};
};

using BroadcastMetricCallback = std::function<void(const BroadcastMetric&)>;

// Immutable value copied from management-owned session state before routing.
class BroadcastTarget {
public:
    explicit BroadcastTarget(
        std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint,
        gamenet::game_session::SessionBinding binding = {},
        bool eligible = true) noexcept
        : endpoint_(std::move(endpoint)),
          binding_(std::move(binding)),
          eligible_(eligible) {}

    gamenet::transport::TransportSessionId id() const noexcept {
        return endpoint_ ? endpoint_->id()
                         : gamenet::transport::TransportSessionId{};
    }
    const std::shared_ptr<gamenet::transport::TransportEndpoint>& endpoint() const noexcept {
        return endpoint_;
    }
    const gamenet::game_session::SessionBinding& binding() const noexcept {
        return binding_;
    }
    bool eligible() const noexcept {
        return eligible_ && (!binding_.tracked() || binding_.isCurrent());
    }

private:
    std::shared_ptr<gamenet::transport::TransportEndpoint> endpoint_;
    gamenet::game_session::SessionBinding binding_;
    bool eligible_{true};
};

class BroadcastPlan {
public:
    BroadcastPlan(const BroadcastPlan&) = delete;
    BroadcastPlan& operator=(const BroadcastPlan&) = delete;
    BroadcastPlan(BroadcastPlan&&) noexcept = default;
    BroadcastPlan& operator=(BroadcastPlan&&) noexcept = default;

    const std::shared_ptr<const std::string>& payload() const noexcept { return payload_; }
    std::size_t batchCount() const noexcept { return batches_.size(); }
    std::size_t accepted() const noexcept { return accepted_; }
    std::size_t dropped() const noexcept { return dropped_; }
    std::size_t reasonCount(BroadcastReason reason) const noexcept {
        const auto index = reasonIndex(reason);
        return index < reasonCounts_.size() ? reasonCounts_[index] : 0;
    }

private:
    struct LoopBatch {
        gamenet::net::EventLoopExecutor ownerExecutor;
        std::vector<std::shared_ptr<gamenet::transport::TransportEndpoint>> endpoints;
    };

    BroadcastPlan() = default;

    std::shared_ptr<const std::string> payload_;
    std::vector<LoopBatch> batches_;
    std::size_t accepted_{};
    std::size_t dropped_{};
    BroadcastPriority priority_{BroadcastPriority::Normal};
    std::array<std::size_t, kBroadcastReasonCount> reasonCounts_{};

    friend class BroadcastRouter;
    friend class BroadcastDispatcher;
};

}  // namespace gamenet::broadcast
