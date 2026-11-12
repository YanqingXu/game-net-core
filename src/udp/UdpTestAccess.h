// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Non-installed lifetime observation, never a public runtime operation.
#include "gamenet/udp/UdpSocket.h"
namespace gamenet::udp::detail {
class UdpTestAccess final {
  public:
    static void beforeDatagramCommit(void (*hook)(void*), void* context);
    static void failNextPreparation();
    static void failNextRead(int error);
    static void failNextWrite(int error);
    static std::weak_ptr<void> state(const UdpSocket& socket) { return socket.state_; }
};
} // namespace gamenet::udp::detail
