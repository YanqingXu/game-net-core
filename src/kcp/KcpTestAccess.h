// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/kcp/KcpTransport.h"
namespace gamenet::kcp::detail {
class KcpTestAccess final {
  public:
    static std::weak_ptr<void> state(const KcpTransport&);
    static void input(KcpTransport&, KcpIdentity, std::string_view, bool truncated = false);
    static void advance(KcpTransport&, std::chrono::milliseconds);
    static void exhaust(KcpTransport&, KcpIdentity);
    static udp::UdpSendAdmission wire(KcpTransport&, KcpIdentity, std::string_view);
};
} // namespace gamenet::kcp::detail
