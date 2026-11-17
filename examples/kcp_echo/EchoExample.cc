// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Implements intents/modules/kcp_endpoint.intent.md; one owner and finite ACK-service window.
#include "EchoExample.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#include <charconv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace gamenet::example {
namespace {
std::uint32_t number(std::string_view text, std::uint32_t minimum, std::uint32_t maximum) {
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("invalid numeric argument");
    return value;
}
bool clean(const kcp::KcpSnapshot& s) {
    return s.phase == udp::UdpPhase::Stopped && s.opened == s.closed &&
           s.accepted == s.acknowledged + s.cancelled + s.failed &&
           !s.sessions && !s.pendingMessages && !s.pendingBytes && !s.protocolBytes &&
           !s.scheduledPosts && !s.udp.peers && !s.udp.pendingDatagrams &&
           !s.udp.pendingBytes && !s.udp.scheduledPosts && !s.udp.socket.nativeOperations &&
           !s.udp.socket.receiveStorageBytes && !s.callbackExceptions;
}
} // namespace

int runKcpEcho(EchoRole role, int argc, char** argv) {
    const bool server = role == EchoRole::Server;
    if (argc < 6 || argc > (server ? 7 : 8)) {
        std::cerr << "Usage: " << argv[0]
                  << " bind-ip bind-port peer-ip peer-port fresh-conv "
                  << (server ? "[deadline-ms]" : "[message] [deadline-ms]") << '\n';
        return 2;
    }
    try {
        const auto localPort = static_cast<std::uint16_t>(number(argv[2], 0, 65535));
        const auto peerPort = static_cast<std::uint16_t>(number(argv[4], 1, 65535));
        const auto conv = number(argv[5], 1, std::numeric_limits<std::uint32_t>::max());
        const auto deadline = std::chrono::milliseconds(
            argc == (server ? 7 : 8) ? number(argv[argc - 1], 100, 60000) : (server ? 10000 : 3000));
        const std::string message = !server && argc >= 7 ? argv[6] : "hello KCP";
        kcp::KcpOptions options;
        if (message.size() > options.maxMessageBytes) throw std::invalid_argument("message too large");
        // Construct the loop first: Windows numeric address conversion requires Winsock startup.
        net::EventLoop loop;
        const net::InetAddress local(argv[1], localPort), peer(argv[3], peerPort);
        if (local.family() != peer.family()) throw std::invalid_argument("address family mismatch");
        unsigned received = 0;
        std::size_t receivedBytes = 0;
        bool failed = false, stopped = false;
        std::shared_ptr<kcp::KcpTransport> service;
        std::shared_ptr<transport::TransportEndpoint> endpoint;
        // Keep every callback capture alive while an exceptional startup path drains.
        try {
            service = kcp::KcpTransport::create(
                &loop, local,
                {.message = [&](auto session, auto bytes) {
                     if (!endpoint || endpoint->id().value != session->identity().peer.id || ++received != 1) {
                         failed = true;
                         (void)service->stop();
                         return;
                     }
                     receivedBytes = bytes.size();
                     if (server) {
                         if (endpoint->send(bytes) != transport::EndpointResult::Accepted) {
                             failed = true;
                             (void)service->stop();
                         }
                     } else if (bytes != message) {
                         failed = true;
                         (void)service->stop();
                     }
                 },
                 .sendComplete = [&](auto, auto completion) {
                     if (completion.status != kcp::KcpSendStatus::Acknowledged) failed = true;
                     // Client remains alive to serve ACKs for the echo, which has no close handshake.
                     if (server || failed) (void)service->stop();
                 },
                 .stopped = [&](auto snapshot) {
                     stopped = true;
                     const bool zero = clean(snapshot);
                     failed = failed || !zero || received != 1 || snapshot.accepted != 1 ||
                              snapshot.acknowledged != 1 || snapshot.cancelled || snapshot.failed;
                     std::cout << "KCP_ECHO_SUMMARY " << (server ? "server" : "client") << ' '
                               << (failed ? "failed" : "passed") << ' ' << received << ' '
                               << snapshot.accepted << ' ' << snapshot.acknowledged << ' '
                               << snapshot.cancelled << ' ' << snapshot.failed << ' ' << unsigned(zero)
                               << ' ' << receivedBytes << std::endl;
                     loop.quit();
                 }}, options);
            if (service->start() != kcp::KcpResult::Accepted) {
                failed = true;
                (void)service->stop();
                if (service->snapshot().phase != udp::UdpPhase::Stopped) loop.loop();
                return 3;
            }
            const auto admission = service->openSession(peer, conv);
            if (!admission.session) {
                failed = true;
                (void)service->stop();
                loop.loop();
                return 3;
            }
            endpoint = std::make_shared<kcp_transport::KcpTransportEndpoint>(admission.session);
            std::cout << "KCP_ECHO_READY " << service->localAddress().toIpPort() << ' '
                      << endpoint->id().value << ' ' << conv << std::endl;
            if (!server && endpoint->send(message) != transport::EndpointResult::Accepted) {
                failed = true;
                (void)service->stop();
            }
            const auto timer = loop.tryRunAfter(deadline, [&] {
                const auto snapshot = service->snapshot();
                if (received != 1 || snapshot.acknowledged != 1) {
                    failed = true;
                    std::cerr << "KCP_ECHO_DEADLINE: no complete acknowledged exchange\n";
                }
                (void)service->stop();
            });
            if (timer.result != net::PostResult::Accepted) {
                failed = true;
                (void)service->stop();
            }
            loop.loop();
            return failed || !stopped ? 1 : 0;
        } catch (...) {
            failed = true;
            if (service) {
                (void)service->stop();
                if (service->snapshot().phase != udp::UdpPhase::Stopped) loop.loop();
            }
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "KCP_ECHO_ERROR: " << error.what() << '\n';
        return 2;
    }
}
} // namespace gamenet::example
