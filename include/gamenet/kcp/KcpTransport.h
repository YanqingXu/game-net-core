// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// 可选标准 KCP 消息会话；ACK 只表示对端协议接收，不表示业务处理。
#include "gamenet/udp/UdpPeer.h"

namespace gamenet::kcp {
namespace detail {
struct KcpState;
struct KcpGate;
class KcpTestAccess;
} // namespace detail
using KcpResult = udp::UdpResult;
enum class KcpCloseReason {
    Normal,
    GoingAway,
    Timeout,
    IdleTimeout,
    ProtocolError,
    ResourceExhausted,
    SchedulingRejected,
    TransportError
};
enum class KcpSendStatus { Acknowledged, Cancelled, Failed };
struct KcpOptions {
    udp::UdpPeerOptions udp{};
    std::size_t maxSessions{32}, maxSessionOpenings{4096};
    std::size_t mtu{1200}, sendWindow{32}, receiveWindow{128};
    std::size_t maxSendSegments{64}, maxAckEntries{256};
    std::size_t maxMessageBytes{16 * 1024};
    std::size_t maxPendingMessagesPerSession{32}, maxPendingBytesPerSession{64 * 1024};
    std::size_t maxPendingMessages{256}, maxPendingBytes{1024 * 1024};
    std::size_t maxProtocolBytes{16 * 1024 * 1024};
    std::size_t maxSessionsPerTurn{8}, maxMessagesPerSessionTurn{8};
    std::chrono::milliseconds interval{20}, sendTimeout{10000}, idleTimeout{30000};
    std::uint32_t maxTransmissions{10};
    void validate() const;
};
struct KcpIdentity {
    udp::UdpPeerIdentity peer{};
    std::uint32_t conv{};
    auto operator<=>(const KcpIdentity&) const = default;
};
struct KcpSendAdmission {
    KcpResult result{KcpResult::Stopped};
    std::uint64_t id{};
};
struct KcpSendCompletion {
    std::uint64_t id{};
    KcpSendStatus status{KcpSendStatus::Failed};
    std::size_t bytes{};
};
struct KcpSnapshot {
    udp::UdpPhase phase{udp::UdpPhase::Created};
    std::size_t sessions{}, pendingMessages{}, pendingBytes{}, protocolBytes{}, scheduledPosts{};
    std::size_t convHistory{}, peakProtocolBytes{}, maxSessionsVisitedPerTurn{},
        maxMessagesPerTurn{};
    std::uint64_t opened{}, closed{}, accepted{}, acknowledged{}, cancelled{}, failed{};
    std::uint64_t received{}, invalidDatagrams{}, ackBudgetDrops{}, outputDrops{},
        callbackExceptions{};
    udp::UdpPeerSnapshot udp{};
};
class KcpSession final {
  public:
    KcpIdentity identity() const noexcept;
    net::InetAddress address() const;
    net::EventLoopExecutor ownerExecutor() const noexcept;
    bool isOpen() const noexcept;
    // Owner-only; Accepted is copied into bounded protocol storage. No inline callback.
    KcpSendAdmission send(std::string_view bytes);
    KcpResult close(KcpCloseReason = KcpCloseReason::Normal);
    gamenet::DispatchResult requestClose(KcpCloseReason = KcpCloseReason::Normal) noexcept;

  private:
    friend struct detail::KcpState;
    KcpSession(std::weak_ptr<detail::KcpState>, std::shared_ptr<detail::KcpGate>);
    std::weak_ptr<detail::KcpState> state_;
    std::shared_ptr<detail::KcpGate> gate_;
};
struct KcpSessionAdmission {
    KcpResult result{KcpResult::Stopped};
    std::shared_ptr<KcpSession> session;
};
struct KcpCallbacks {
    // Borrowed payload expires at return. All callbacks are owner-only, may reenter/release/throw.
    std::function<void(std::shared_ptr<KcpSession>, std::string_view)> message;
    std::function<void(KcpIdentity, KcpSendCompletion)> sendComplete;
    std::function<void(KcpIdentity, KcpCloseReason)> closed;
    std::function<void(KcpSnapshot)> stopped;
};
// Owner-only create/start/openSession/address/snapshot. Thread-safe stop/destruction.
// Keep the owner running/draining until stopped. Handles may outlive transport/loop.
class KcpTransport final {
  public:
    static std::shared_ptr<KcpTransport> create(net::EventLoop*, const net::InetAddress&,
                                                KcpCallbacks = {}, KcpOptions = {});
    ~KcpTransport();
    KcpTransport(const KcpTransport&) = delete;
    KcpTransport& operator=(const KcpTransport&) = delete;
    KcpResult start();
    // Known peer only; nonzero conv must never repeat during this transport's lifetime.
    KcpSessionAdmission openSession(const net::InetAddress&, std::uint32_t conv);
    KcpResult stop() noexcept;
    net::InetAddress localAddress() const;
    KcpSnapshot snapshot() const;

  private:
    friend class detail::KcpTestAccess;
    explicit KcpTransport(std::shared_ptr<detail::KcpState>);
    std::shared_ptr<detail::KcpState> state_;
};
} // namespace gamenet::kcp
