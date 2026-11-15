// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp/KcpTransport.h"
#include "support/TestAssert.h"
#include <iostream>
using namespace gamenet;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    GAMENET_TEST_ASSERT(argc == 4);
    const auto host = std::string(argv[1]) == "6" ? "::1" : "127.0.0.1";
    net::EventLoop loop;
    unsigned received = 0, acked = 0, closed = 0, stopped = 0;
    std::shared_ptr<kcp::KcpTransport> server;
    server = kcp::KcpTransport::create(
        &loop, net::InetAddress(host, 0),
        {.message =
             [&](auto p, auto bytes) {
                 ++received;
                 GAMENET_TEST_ASSERT(p->send(bytes).id);
             },
         .sendComplete =
             [&](auto, auto c) {
                 GAMENET_TEST_ASSERT(c.status == kcp::KcpSendStatus::Acknowledged);
                 if (++acked == 4)
                     (void)server->stop();
             },
         .closed = [&](auto, auto) { ++closed; },
         .stopped =
             [&](auto s) {
                 GAMENET_TEST_ASSERT(!s.sessions && !s.pendingMessages && !s.pendingBytes &&
                                     !s.protocolBytes && !s.scheduledPosts &&
                                     !s.udp.socket.nativeOperations &&
                                     !s.udp.socket.receiveStorageBytes);
                 ++stopped;
                 std::cout << "{\"status\":\"passed\",\"received\":" << received
                           << ",\"acknowledged\":" << acked << ",\"closed\":" << closed
                           << ",\"stopped\":" << stopped << ",\"residual\":0}" << std::endl;
                 loop.quit();
             }});
    GAMENET_TEST_ASSERT(server->start() == kcp::KcpResult::Accepted);
    GAMENET_TEST_ASSERT(
        server
            ->openSession(net::InetAddress(host, static_cast<std::uint16_t>(std::stoi(argv[2]))),
                          static_cast<std::uint32_t>(std::stoul(argv[3])))
            .session);
    std::cout << "PORT " << server->localAddress().port() << std::endl;
    loop.runAfter(20s, [] { GAMENET_TEST_FAIL("independent KCP echo timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(received == 4 && acked == 4 && closed == 1 && stopped == 1 &&
                        !loop.attachedLifecycleNodeCount());
}
