// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Non-installed owner-only controls for deterministic sequence contracts.
#include "gamenet/udp/UdpPeer.h"
#include <functional>
#include <utility>
namespace gamenet::udp::detail {
class UdpPeerTestAccess {
  public:
    static std::weak_ptr<void> state(const UdpPeerServer&);
    // One-shot owner checkpoints; callbacks run without admission locks.
    static void beforeSocketStart(UdpPeerServer&, std::function<void()>);
    static void afterScan(UdpPeerServer&, std::function<void()>);
    static void receive(UdpPeerServer&, const net::InetAddress&, std::string_view, bool = false);
    static void expire(UdpPeerServer&);
    static std::uint64_t nativeId(const UdpPeerServer&);
    static void replay(UdpPeerServer&, std::uint64_t);
    static std::pair<std::size_t, std::size_t> pending(const UdpPeerServer&, UdpPeerIdentity);
};
} // namespace gamenet::udp::detail
