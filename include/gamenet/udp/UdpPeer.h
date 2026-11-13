// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// 显式 UDP 对端会话；身份只隔离本地生命周期，不认证网络来源。
#include "gamenet/core/DispatchResult.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/udp/UdpSocket.h"
#include <chrono>
#include <compare>

namespace gamenet::udp {
namespace detail {
struct UdpPeerState;
struct UdpPeerGate;
class UdpPeerTestAccess;
} // namespace detail
struct UdpPeerIdentity {
    std::uint64_t id{};
    std::size_t slot{};
    std::uint64_t generation{};
    auto operator<=>(const UdpPeerIdentity&) const = default;
};
struct UdpPeerOptions {
    UdpOptions socket{};
    std::size_t maxPeers{128};
    std::size_t maxPendingDatagramsPerPeer{32};
    std::size_t maxPendingBytesPerPeer{64 * 1024};
    std::size_t maxPeersPerTurn{32};
    std::size_t maxCompletionsPerTurn{32};
    std::chrono::milliseconds idleTimeout{30000};
    void validate() const;
};
struct UdpPeerSnapshot {
    UdpPhase phase{UdpPhase::Created};
    std::size_t peers{}, pendingDatagrams{}, pendingBytes{}, scheduledPosts{};
    std::uint64_t opened{}, closed{}, acceptedDatagrams{}, acceptedBytes{};
    std::uint64_t sentDatagrams{}, sentBytes{}, cancelledDatagrams{}, cancelledBytes{};
    std::uint64_t failedDatagrams{}, failedBytes{}, unknownDatagrams{}, receivedDatagrams{};
    std::uint64_t ignoredCompletions{}, callbackExceptions{}, idleClosed{};
    std::size_t maxPeersVisitedPerTurn{}, maxCompletionsPerTurn{};
    UdpSnapshot socket{};
};
enum class UdpPeerCloseReason {
    Normal,
    GoingAway,
    ProtocolError,
    Replaced,
    IdleTimeout,
    Overloaded
};
class UdpPeer final {
  public:
    UdpPeerIdentity identity() const noexcept;
    net::InetAddress address() const;
    std::uint64_t id() const noexcept;
    net::EventLoopExecutor ownerExecutor() const noexcept;
    UdpResult send(std::string_view bytes);
    UdpSendAdmission sendDatagram(std::string_view bytes);
    UdpResult close(UdpPeerCloseReason reason);
    gamenet::DispatchResult requestClose(UdpPeerCloseReason reason) noexcept;
    bool isOpen() const noexcept;

  private:
    friend struct detail::UdpPeerState;
    UdpPeer(std::weak_ptr<detail::UdpPeerState>, std::shared_ptr<detail::UdpPeerGate>);
    std::weak_ptr<detail::UdpPeerState> state_;
    std::shared_ptr<detail::UdpPeerGate> gate_;
};
struct UdpPeerAdmission {
    UdpResult result{UdpResult::Stopped};
    std::shared_ptr<UdpPeer> peer;
};
struct UdpPeerCallbacks {
    // Views expire at return. All callbacks are owner-only and may
    // reenter/release.
    std::function<void(std::shared_ptr<UdpPeer>, std::string_view, bool)> datagram;
    std::function<void(UdpPeerIdentity, UdpSendCompletion)> sendComplete;
    std::function<void(UdpPeerIdentity, UdpPeerCloseReason)> closed;
    std::function<void(UdpPeerSnapshot)> stopped;
};
// Owner: create/start/openPeer/address/snapshot. Thread-safe: stop/destruction.
// Keep owner alive until stopped; peer handles may safely outlive server/loop.
class UdpPeerServer final {
  public:
    static std::shared_ptr<UdpPeerServer> create(net::EventLoop*, const net::InetAddress&,
                                                 UdpPeerCallbacks = {}, UdpPeerOptions = {});
    ~UdpPeerServer();
    UdpPeerServer(const UdpPeerServer&) = delete;
    UdpPeerServer& operator=(const UdpPeerServer&) = delete;
    UdpResult start();
    UdpPeerAdmission openPeer(const net::InetAddress&);
    UdpResult stop() noexcept;
    net::InetAddress localAddress() const;
    UdpPeerSnapshot snapshot() const;

  private:
    friend class detail::UdpPeerTestAccess;
    explicit UdpPeerServer(std::shared_ptr<detail::UdpPeerState>);
    std::shared_ptr<detail::UdpPeerState> state_;
};
} // namespace gamenet::udp
