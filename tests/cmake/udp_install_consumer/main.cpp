// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/core/net/EventLoop.h>
#ifdef CONSUMER_UDP
#include <gamenet/udp/UdpSocket.h>
#endif
int main() {
    gamenet::net::EventLoop loop;
#ifdef CONSUMER_UDP
    bool stopped = false;
    auto socket = gamenet::udp::UdpSocket::create(
        &loop, gamenet::net::InetAddress(0, true), {.stopped = [&](auto s) {
            stopped = s.phase == gamenet::udp::UdpPhase::Stopped && !s.nativeOperations &&
                      !s.pendingBytes;
            loop.quit();
        }});
    if (socket->start() != gamenet::udp::UdpResult::Accepted)
        return 1;
    (void)socket->stop();
    loop.loop();
    return stopped ? 0 : 2;
#else
    return 0;
#endif
}
