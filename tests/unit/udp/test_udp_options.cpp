// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/udp/UdpPeer.h"
#include "gamenet/udp/UdpSocket.h"
#include "support/TestAssert.h"
#include <stdexcept>
using namespace gamenet;
static void unavailableLifecycle() {
    net::EventLoop loop({.maxLifecycleNodes = 0});
    unsigned closed = 0;
    auto socket = udp::UdpSocket::create(&loop, net::InetAddress(0, true), {.stopped = [&](auto s) {
        ++closed;
        GAMENET_TEST_ASSERT(s.phase == udp::UdpPhase::Stopped && !s.nativeOperations &&
                            !s.pendingDatagrams && !s.receiveStorageBytes);
    }});
    GAMENET_TEST_ASSERT(socket->start() == udp::UdpResult::OwnerUnavailable && closed == 1 &&
                        !loop.attachedLifecycleNodeCount());
}
int main() {
    udp::UdpPeerOptions{}.validate();
    for (unsigned n = 0; n < 8; ++n) {
        udp::UdpPeerOptions p;
        switch (n) {
        case 0:
            p.maxPeers = 0;
            break;
        case 1:
            p.maxPeers = 4097;
            break;
        case 2:
            p.maxPendingDatagramsPerPeer = p.socket.maxPendingDatagrams + 1;
            break;
        case 3:
            p.maxPendingBytesPerPeer = p.socket.maxDatagramBytes - 1;
            break;
        case 4:
            p.maxPendingBytesPerPeer = p.socket.maxPendingBytes + 1;
            break;
        case 5:
            p.maxPeersPerTurn = 0;
            break;
        case 6:
            p.maxCompletionsPerTurn = 0;
            break;
        case 7:
            p.idleTimeout = std::chrono::milliseconds(0);
            break;
        }
        bool rejected = false;
        try {
            p.validate();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GAMENET_TEST_ASSERT(rejected);
    }
    unavailableLifecycle();
    udp::UdpOptions{}.validate();
    for (unsigned i = 0; i < 9; ++i) {
        udp::UdpOptions o;
        switch (i) {
        case 0:
            o.maxDatagramBytes = 0;
            break;
        case 1:
            o.maxDatagramBytes = 65508;
            break;
        case 2:
            o.maxPendingDatagrams = 0;
            break;
        case 3:
            o.maxPendingBytes = 1199;
            break;
        case 4:
            o.maxReceiveDatagramsPerBatch = 0;
            break;
        case 5:
            o.maxReceiveBytesPerBatch = 1199;
            break;
        case 6:
            o.maxSendDatagramsPerBatch = 0;
            break;
        case 7:
            o.maxSendBytesPerBatch = 1199;
            break;
        case 8:
            o.maxPendingDatagrams = 65537;
            break;
        }
        bool threw = false;
        try {
            o.validate();
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        GAMENET_TEST_ASSERT(threw);
    }
    net::EventLoop loop;
    bool threw = false;
    try {
        (void)udp::UdpSocket::create(nullptr, net::InetAddress(0, true));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    GAMENET_TEST_ASSERT(threw);
    auto socket = udp::UdpSocket::create(&loop, net::InetAddress(0, true));
    GAMENET_TEST_ASSERT(socket->sendTo(net::InetAddress(1, true), "").result ==
                        udp::UdpResult::Stopped);
    GAMENET_TEST_ASSERT(socket->stop() == udp::UdpResult::Stopped &&
                        socket->start() == udp::UdpResult::Stopped);
}
