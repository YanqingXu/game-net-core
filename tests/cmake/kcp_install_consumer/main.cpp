// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/core/net/EventLoop.h"
#ifdef CONSUMER_KCP
#include "../../../third_party/kcp/ikcp.h"
#include "gamenet/kcp/KcpTransport.h"
#endif
#ifdef CONSUMER_KCP_ENDPOINT
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#endif
int main() {
    gamenet::net::EventLoop loop;
#ifdef CONSUMER_KCP
    bool stopped = false;
    auto s = gamenet::kcp::KcpTransport::create(
        &loop, gamenet::net::InetAddress("127.0.0.1", 0), {.stopped = [&](auto snap) {
            stopped = !snap.pendingMessages && !snap.protocolBytes;
            loop.quit();
        }});
    if (s->start() != gamenet::kcp::KcpResult::Accepted)
        return 1;
    auto session = s->openSession(gamenet::net::InetAddress("127.0.0.1", 9), 77).session;
    if (!session)
        return 3;
#ifdef CONSUMER_KCP_ENDPOINT
    gamenet::kcp_transport::KcpTransportEndpoint endpoint(session);
    gamenet::transport::TransportEndpoint& abstract = endpoint;
    if (!abstract.isOpen() || abstract.id().value != session->identity().peer.id ||
        abstract.ownerExecutor().id() != loop.executor().id() ||
        abstract.send("installed") != gamenet::transport::EndpointResult::Accepted ||
        abstract.close(gamenet::transport::CloseReason::Normal) != gamenet::transport::EndpointResult::Accepted)
        return 5;
#endif
    // An application-owned upstream copy must retain its own malloc hooks.
    auto* independent = ikcp_create(88, nullptr);
    if (!independent)
        return 4;
    ikcp_release(independent);
    (void)s->stop();
    loop.loop();
    if (!stopped)
        return 2;
#endif
}
