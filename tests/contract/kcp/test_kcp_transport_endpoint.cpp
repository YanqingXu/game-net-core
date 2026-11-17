// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Contracts for intents/modules/kcp_endpoint.intent.md and KCP-C2 ownership/thread rules.
#include "gamenet/kcp_transport/KcpTransportEndpoint.h"
#include "support/KcpSequence.h"
#include "support/TestAssert.h"
#include <array>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace gamenet;
using namespace std::chrono_literals;
using Endpoint = kcp_transport::KcpTransportEndpoint;
using Reason = transport::CloseReason;
using Result = transport::EndpointResult;
static_assert(!std::is_copy_constructible_v<Endpoint> && !std::is_move_constructible_v<Endpoint>);

void settled(const kcp::KcpSnapshot& s) {
    GAMENET_TEST_ASSERT(s.phase == udp::UdpPhase::Stopped);
    GAMENET_TEST_ASSERT(s.accepted == s.acknowledged + s.cancelled + s.failed);
    GAMENET_TEST_ASSERT(s.opened == s.closed && !s.sessions && !s.pendingMessages &&
                        !s.pendingBytes && !s.protocolBytes && !s.scheduledPosts &&
                        !s.udp.peers && !s.udp.pendingDatagrams && !s.udp.pendingBytes &&
                        !s.udp.scheduledPosts && !s.udp.socket.nativeOperations &&
                        !s.udp.socket.receiveStorageBytes && !s.callbackExceptions);
}

void closeMapping(Reason reason, kcp::KcpCloseReason expected) {
    std::shared_ptr<transport::TransportEndpoint> endpoint;
    std::uint64_t identity = 0;
    std::weak_ptr<void> weakState;
    {
        net::EventLoop loop({.maxPendingFunctors = 1, .reservedPendingFunctors = 0,
                             .maxFunctorsPerIteration = 1});
        test::RawUdp remote;
        unsigned completions = 0, closed = 0, stopped = 0;
        std::shared_ptr<kcp::KcpTransport> service;
        kcp::KcpOptions options;
        options.maxPendingMessagesPerSession = 1;
        options.maxMessageBytes = 32;
        service = kcp::KcpTransport::create(
            &loop, net::InetAddress("127.0.0.1", 0),
            {.sendComplete = [&](auto id, auto receipt) {
                 GAMENET_TEST_ASSERT(id.peer.id == identity && receipt.id == 1 &&
                                     receipt.bytes == 7 && ++completions == 1);
                 const bool cancellation = expected == kcp::KcpCloseReason::Normal ||
                                           expected == kcp::KcpCloseReason::GoingAway;
                 GAMENET_TEST_ASSERT(receipt.status == (cancellation ? kcp::KcpSendStatus::Cancelled
                                                                     : kcp::KcpSendStatus::Failed));
             },
             .closed = [&](auto id, auto actual) {
                 GAMENET_TEST_ASSERT(id.peer.id == identity && actual == expected && ++closed == 1);
                 (void)service->stop();
             },
             .stopped = [&](auto snapshot) {
                 settled(snapshot);
                 GAMENET_TEST_ASSERT(snapshot.accepted == 1 && ++stopped == 1);
                 loop.quit();
             }}, options);
        weakState = kcp::detail::KcpTestAccess::state(*service);
        GAMENET_TEST_ASSERT(service->start() == kcp::KcpResult::Accepted);
        auto session = service->openSession(remote.address(), 77).session;
        GAMENET_TEST_ASSERT(session);
        endpoint = std::make_shared<Endpoint>(session);
        identity = session->identity().peer.id;
        GAMENET_TEST_ASSERT(identity && endpoint->id().value == identity && endpoint->isOpen() &&
                            endpoint->ownerExecutor().id() == session->ownerExecutor().id());
        GAMENET_TEST_ASSERT(endpoint->send(std::string(33, 'x')) == Result::Overloaded &&
                            !service->snapshot().accepted);
        GAMENET_TEST_ASSERT(endpoint->send("pending") == Result::Accepted &&
                            endpoint->send("") == Result::Overloaded &&
                            service->snapshot().accepted == 1 && service->snapshot().pendingBytes == 7);
        loop.runAfter(5s, [] { GAMENET_TEST_FAIL("KCP endpoint close timeout"); });
        // No ordinary queue capacity is available to the foreign lifecycle close.
        const auto fill = loop.executor().post([] {});
        GAMENET_TEST_ASSERT(fill == net::PostResult::Accepted || fill == net::PostResult::QueueFull);
        GAMENET_TEST_ASSERT(loop.executor().post([] {}) == net::PostResult::QueueFull);
        std::thread foreign([&] {
            GAMENET_TEST_ASSERT(endpoint->isOpen());
            GAMENET_TEST_ASSERT(endpoint->send("foreign") == Result::WrongThread &&
                                endpoint->close(Reason::Normal) == Result::WrongThread);
            GAMENET_TEST_ASSERT(endpoint->requestClose(reason) == DispatchResult::Accepted &&
                                !endpoint->isOpen() &&
                                endpoint->requestClose(Reason::Normal) == DispatchResult::EndpointClosed);
        });
        foreign.join();
        GAMENET_TEST_ASSERT(endpoint->close(Reason::Normal) == Result::Closed);
        loop.loop();
        GAMENET_TEST_ASSERT(completions == 1 && closed == 1 && stopped == 1);
        service.reset();
        GAMENET_TEST_ASSERT(weakState.expired());
    }
    GAMENET_TEST_ASSERT(endpoint->id().value == identity && !endpoint->isOpen() &&
                        !endpoint->ownerExecutor().available() &&
                        endpoint->send("after-loop") == Result::Closed &&
                        endpoint->close(Reason::Normal) == Result::Closed &&
                        endpoint->requestClose(Reason::Normal) == DispatchResult::EndpointClosed);
}

void replacement() {
    net::EventLoop loop;
    test::RawUdp remote;
    unsigned stopped = 0, closed = 0;
    const auto done = [&](auto snapshot) {
        settled(snapshot);
        if (++stopped == 2) loop.quit();
    };
    auto first = kcp::KcpTransport::create(&loop, net::InetAddress("127.0.0.1", 0),
        {.closed = [&](auto, auto) { ++closed; }, .stopped = done});
    auto second = kcp::KcpTransport::create(&loop, net::InetAddress("127.0.0.1", 0),
        {.closed = [&](auto, auto) { ++closed; }, .stopped = done});
    GAMENET_TEST_ASSERT(first->start() == kcp::KcpResult::Accepted &&
                        second->start() == kcp::KcpResult::Accepted);
    auto old = std::make_shared<Endpoint>(first->openSession(remote.address(), 11).session);
    GAMENET_TEST_ASSERT(old->close(Reason::Replaced) == Result::Accepted);
    auto current = std::make_shared<Endpoint>(first->openSession(remote.address(), 12).session);
    Endpoint sameConv(second->openSession(remote.address(), 12).session);
    GAMENET_TEST_ASSERT(old->id() != current->id() && sameConv.id() != current->id());
    GAMENET_TEST_ASSERT(!old->isOpen() && old->send("stale") == Result::Closed &&
                        old->requestClose(Reason::Overloaded) == DispatchResult::EndpointClosed &&
                        current->isOpen() && sameConv.isOpen());
    GAMENET_TEST_ASSERT(current->send("") == Result::Accepted &&
                        current->close(Reason::Normal) == Result::Accepted &&
                        sameConv.close(Reason::GoingAway) == Result::Accepted);
    (void)first->stop();
    (void)second->stop();
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("KCP endpoint generation timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(closed == 3 && stopped == 2 && !current->isOpen());
}

void exchange(bool ipv6) {
    net::EventLoop loop;
    const net::InetAddress bind(ipv6 ? "::1" : "127.0.0.1", 0);
    const std::array<std::string, 2> payloads{std::string(6144, 'x'), std::string{}};
    std::shared_ptr<kcp::KcpTransport> client, server;
    std::shared_ptr<transport::TransportEndpoint> left, right;
    std::array<std::set<std::uint64_t>, 2> completions;
    unsigned sent = 0, echoed = 0, stopped = 0;
    bool stopping = false;
    const auto maybeStop = [&] {
        if (!stopping && echoed == 2 && completions[0].size() == 2 && completions[1].size() == 2) {
            stopping = true;
            // Last-wrapper release from a completion callback must not be retained by the adapter.
            client.reset();
            right.reset();
            (void)server->stop();
        }
    };
    const auto terminal = [&](unsigned side, auto receipt) {
        GAMENET_TEST_ASSERT(receipt.status == kcp::KcpSendStatus::Acknowledged &&
                            completions[side].insert(receipt.id).second);
        maybeStop();
    };
    const auto done = [&](auto snapshot) {
        settled(snapshot);
        GAMENET_TEST_ASSERT(snapshot.accepted == 2 && snapshot.acknowledged == 2);
        if (++stopped == 2) loop.quit();
    };
    client = kcp::KcpTransport::create(&loop, bind,
        {.message = [&](auto, auto bytes) {
             GAMENET_TEST_ASSERT(echoed < 2 && bytes == payloads[echoed++]);
             if (echoed == 2) left.reset();
             maybeStop();
         }, .sendComplete = [&](auto, auto receipt) { terminal(0, receipt); }, .stopped = done});
    server = kcp::KcpTransport::create(&loop, bind,
        {.message = [&](auto, auto bytes) {
             GAMENET_TEST_ASSERT(sent < 2 && bytes == payloads[sent++]);
             GAMENET_TEST_ASSERT(right->send(bytes) == Result::Accepted);
         }, .sendComplete = [&](auto, auto receipt) { terminal(1, receipt); }, .stopped = done});
    GAMENET_TEST_ASSERT(client->start() == kcp::KcpResult::Accepted &&
                        server->start() == kcp::KcpResult::Accepted);
    const auto weakClient = kcp::detail::KcpTestAccess::state(*client);
    left = std::make_shared<Endpoint>(client->openSession(server->localAddress(), 91).session);
    right = std::make_shared<Endpoint>(server->openSession(client->localAddress(), 91).session);
    for (const auto& payload : payloads)
        GAMENET_TEST_ASSERT(left->send(payload) == Result::Accepted);
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("KCP endpoint exchange timeout"); });
    loop.loop();
    GAMENET_TEST_ASSERT(sent == 2 && echoed == 2 && stopped == 2 && weakClient.expired() && !left && !right);
}

int main() {
    bool rejected = false;
    try { Endpoint invalid(nullptr); } catch (const std::invalid_argument&) { rejected = true; }
    GAMENET_TEST_ASSERT(rejected);
    for (auto [reason, expected] : {
             std::pair{Reason::Normal, kcp::KcpCloseReason::Normal},
             std::pair{Reason::GoingAway, kcp::KcpCloseReason::GoingAway},
             std::pair{Reason::ProtocolError, kcp::KcpCloseReason::ProtocolError},
             std::pair{Reason::Replaced, kcp::KcpCloseReason::GoingAway},
             std::pair{Reason::IdleTimeout, kcp::KcpCloseReason::IdleTimeout},
             std::pair{Reason::Overloaded, kcp::KcpCloseReason::ResourceExhausted}})
        closeMapping(reason, expected);
    replacement();
    exchange(false);
    exchange(true);
}
