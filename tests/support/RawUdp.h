// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/udp/UdpSocket.h"
#include "support/TestAssert.h"
#include <string_view>
namespace gamenet::test {
struct RawUdp {
    net::SocketFd fd;
    explicit RawUdp(bool ipv6 = false)
        : fd(net::sockets::createNonblockingDatagram(ipv6 ? AF_INET6 : AF_INET)) {
        GAMENET_TEST_ASSERT(net::sockets::isValid(fd));
        const net::InetAddress local(ipv6 ? "::1" : "127.0.0.1", 0);
        GAMENET_TEST_ASSERT(::bind(fd, local.getSockAddr(), local.getSockAddrLen()) == 0);
    }
    ~RawUdp() { net::sockets::close(fd); }
    RawUdp(const RawUdp&) = delete;
    RawUdp& operator=(const RawUdp&) = delete;
    net::InetAddress address() const { return net::InetAddress(net::sockets::getLocalAddr(fd)); }
    void send(const net::InetAddress& peer, std::string_view bytes) {
        GAMENET_TEST_ASSERT(net::sockets::sendTo(fd, bytes.data(), bytes.size(), peer.getSockAddr(),
                                                 peer.getSockAddrLen()) ==
                            static_cast<ssize_t>(bytes.size()));
    }
};
inline void verifyUdpLedger(const udp::UdpSnapshot& s) {
    GAMENET_TEST_ASSERT(s.acceptedDatagrams == s.sentDatagrams + s.failedDatagrams +
                                                   s.cancelledDatagrams + s.pendingDatagrams);
    GAMENET_TEST_ASSERT(s.acceptedBytes ==
                        s.sentBytes + s.failedBytes + s.cancelledBytes + s.pendingBytes);
    if (s.phase == udp::UdpPhase::Stopped)
        GAMENET_TEST_ASSERT(!s.pendingDatagrams && !s.pendingBytes && !s.nativeOperations &&
                            !s.receiveStorageBytes);
}
} // namespace gamenet::test
