// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/RpcTlsFixture.h"
#include "support/TestAssert.h"
#include <any>
#include <thread>
using namespace gamenet::rpc;
using gamenet::tls::TlsError;
using gamenet::tls::TlsOperation;
using namespace std::chrono_literals;
namespace {
void gateAndEcho(gamenet::test::TlsCertificates& cert) {
    RpcTlsOptions options; options.tls.bioCapacity = 128; options.tls.maxStepsPerDrive = 2;
    gamenet::test::RpcTlsFixture f(cert, options, options); f.fragment = 7; f.forwardWritable = false;
    unsigned callbacks = 0;
    const auto early = f.client->channel()->call(1, "early", 1s, [&](auto) { ++callbacks; });
    GAMENET_TEST_ASSERT(early.status == RpcOperation::NotReady && early.requestId == 0);
    const auto before = f.client->snapshot();
    GAMENET_TEST_ASSERT(before.rpc.pendingRequests == 0 && before.rpc.pendingBytes == 0 &&
        before.tls.acceptedPlaintextBytes == 0 && callbacks == 0);
    GAMENET_TEST_ASSERT(f.client->start() == TlsOperation::Accepted);
    const auto handshaking = f.client->channel()->call(1, "handshaking", 1s, [&](auto) { ++callbacks; });
    GAMENET_TEST_ASSERT(handshaking.status == RpcOperation::NotReady && handshaking.requestId == 0 &&
        f.client->snapshot().rpc.pendingBytes == 0 && f.client->snapshot().tls.acceptedPlaintextBytes == 0);
    const std::string payload = std::string("binary\0", 7) + std::string(4096, 'x');
    f.serverHandler = [&](const auto& request, auto reply) {
        GAMENET_TEST_ASSERT(request.payload == payload && reply({RpcStatus::Ok, request.payload}) == RpcOperation::Accepted);
    };
    f.ready = [&] {
        const auto call = f.client->channel()->call(1, payload, 3s, [&](auto result) {
            GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok && result.payload == payload);
            ++callbacks; (void)f.client->shutdown();
        });
        GAMENET_TEST_ASSERT(call.status == RpcOperation::Accepted && call.requestId == 1);
    };
    f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); });
    const auto s = f.client->snapshot();
    GAMENET_TEST_ASSERT(callbacks == 1 && f.clientReady == 1 && f.serverReady == 1 &&
        f.clientClosed == std::vector{TlsError::Graceful} && f.serverClosed == std::vector{TlsError::Graceful});
    GAMENET_TEST_ASSERT(s.rpc.acceptedCalls == s.rpc.completedCalls && s.rpc.pendingRequests == 0 &&
        s.rpc.pendingBytes == 0 && s.rpc.inboundRequests == 0 && s.tls.discardedPlaintextBytes == 0);
}
void rejectedHandshake(gamenet::test::TlsCertificates& cert) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        RpcTlsOptions options;
        options.peerName = scenario == 0 ? "wrong.local" : "localhost";
        if (scenario == 3) options.tls.handshakeTimeout = 20ms;
        gamenet::test::RpcTlsFixture f(cert, options, {}, scenario == 1, scenario == 2);
        unsigned completions = 0;
        f.runUntil([&] { return !f.clientClosed.empty(); }, scenario != 3);
        const auto result = f.client->channel()->call(1, "closed", 1s, [&](auto) { ++completions; });
        const auto s = f.client->snapshot();
        GAMENET_TEST_ASSERT(result.status == RpcOperation::Closed && result.requestId == 0 &&
            completions == 0 && f.clientReady == 0 && s.rpc.acceptedCalls == 0 &&
            s.rpc.pendingRequests == 0 && s.rpc.pendingBytes == 0 && s.tls.acceptedPlaintextBytes == 0);
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{scenario == 3 ? TlsError::HandshakeTimeout : TlsError::CertificateRejected});
    }
}
void deadlinesCancelAndPending(gamenet::test::TlsCertificates& cert) {
    RpcTlsOptions options; options.rpc.maxPendingRequests = 2; options.rpc.maxPendingBytes = 64;
    gamenet::test::RpcTlsFixture f(cert, options);
    unsigned done = 0, handlers = 0, rejected = 0;
    f.serverHandler = [&](const auto&, auto reply) {
        ++handlers;
        f.loop.runAfter(60ms, [reply] { (void)reply({RpcStatus::Ok, "late"}); });
    };
    f.ready = [&] {
        const auto a = f.client->channel()->call(1, "a", 20ms, [&](auto result) {
            GAMENET_TEST_ASSERT(result.status == RpcStatus::Timeout); ++done;
        });
        const auto b = f.client->channel()->call(1, "b", 1s, [&](auto result) {
            GAMENET_TEST_ASSERT(result.status == RpcStatus::Cancelled); ++done;
        });
        GAMENET_TEST_ASSERT(a.status == RpcOperation::Accepted && b.status == RpcOperation::Accepted);
        const auto full = f.client->channel()->call(1, "c", 1s, [&](auto) { ++rejected; });
        GAMENET_TEST_ASSERT(full.status == RpcOperation::PendingLimit && full.requestId == 0);
        GAMENET_TEST_ASSERT(f.client->channel()->cancel(b.requestId) == RpcOperation::Accepted);
        GAMENET_TEST_ASSERT(f.client->channel()->cancel(b.requestId) == RpcOperation::NotFound);
    };
    f.runUntil([&] { return done == 2 && handlers == 2 && f.client->snapshot().rpc.ignoredResponses == 2; });
    const auto s = f.client->snapshot().rpc;
    GAMENET_TEST_ASSERT(done == 2 && rejected == 0 && s.pendingRequests == 0 && s.pendingBytes == 0 &&
        s.acceptedCalls == 2 && s.completedCalls == 2);
}
void tlsAdmissionBackpressure(gamenet::test::TlsCertificates& cert) {
    RpcTlsOptions options; options.tls.maxPendingWrites = 1;
    gamenet::test::RpcTlsFixture f(cert, options);
    unsigned done = 0, rejected = 0;
    f.serverHandler = [](const auto& request, auto reply) { (void)reply({RpcStatus::Ok, request.payload}); };
    f.clientHandler = [&](const auto&, auto) {
        // The client handler is inside client SSL drive, so its copied write
        // stays queued until this handler unwinds, independent of handshake order.
        const auto first = f.client->channel()->call(1, "one", 1s, [&](auto r) {
            GAMENET_TEST_ASSERT(r.status == RpcStatus::Ok); ++done;
        });
        const auto second = f.client->channel()->call(1, "two", 1s, [&](auto) { ++rejected; });
        GAMENET_TEST_ASSERT(first.status == RpcOperation::Accepted && second.status == RpcOperation::SendRejected);
        GAMENET_TEST_ASSERT(second.requestId == 0 && f.client->snapshot().rpc.pendingRequests == 1);
    };
    f.ready = [&] {
        GAMENET_TEST_ASSERT(f.server->channel()->call(99, "trigger", 1s, [](auto) {}).status == RpcOperation::Accepted);
    };
    f.runUntil([&] { return done == 1; });
    const auto s = f.client->snapshot().rpc;
    GAMENET_TEST_ASSERT(rejected == 0 && s.acceptedCalls == 1 && s.completedCalls == 1 && s.pendingBytes == 0);
}
void tcpBackpressureRecovery(gamenet::test::TlsCertificates& cert) {
    RpcTlsOptions options; options.rpc.maxPayloadBytes = 256 * 1024; options.rpc.maxBufferedBytes = 512 * 1024;
    gamenet::test::RpcTlsFixture f(cert, options, options, false, false, 8192);
    f.forwardWritable = false;
    const std::string payload(256 * 1024, 'b'); unsigned done = 0;
    f.serverHandler = [](const auto& request, auto reply) { GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, request.payload}) == RpcOperation::Accepted); };
    f.ready = [&] {
        GAMENET_TEST_ASSERT(f.client->channel()->call(1, payload, 5s, [&](auto r) {
            GAMENET_TEST_ASSERT(r.status == RpcStatus::Ok && r.payload == payload); ++done;
        }).status == RpcOperation::Accepted);
    };
    f.runUntil([&] { return done == 1; });
    GAMENET_TEST_ASSERT(f.client->snapshot().tls.transportRejections > 0 && f.client->snapshot().rpc.pendingBytes == 0);
}
void disconnectAndExplicitClose(gamenet::test::TlsCertificates& cert) {
    for (bool explicitClose : {false, true}) {
        gamenet::test::RpcTlsFixture f(cert); unsigned done = 0;
        f.serverHandler = [&](const auto&, auto) {
            if (explicitClose) (void)f.client->shutdown(); else f.serverConnection->forceClose();
        };
        f.ready = [&] {
            GAMENET_TEST_ASSERT(f.client->channel()->call(1, "pending", 1s, [&](auto r) {
                GAMENET_TEST_ASSERT(r.status == (explicitClose ? RpcStatus::Shutdown : RpcStatus::Disconnected)); ++done;
            }).status == RpcOperation::Accepted);
        };
        f.runUntil([&] { return done == 1 && !f.clientClosed.empty(); });
        (void)f.client->abort(); f.client->onDisconnect();
        GAMENET_TEST_ASSERT(done == 1 && f.clientClosed.size() == 1 && f.client->snapshot().rpc.pendingRequests == 0 &&
            f.client->snapshot().rpc.inboundRequests == 0);
    }
}
void ownerAndConstruction(gamenet::test::TlsCertificates& cert) {
    gamenet::test::RpcTlsFixture f(cert);
    TlsOperation foreign{};
    std::thread worker([&] { foreign = f.client->start(); }); worker.join();
    GAMENET_TEST_ASSERT(foreign == TlsOperation::WrongThread);
    unsigned callbacks = 0; RpcTlsOptions bad; bad.rpc.maxPendingRequests = 0; bad.peerName = "localhost";
    try {
        (void)RpcTlsSession::create(f.clientConnection, gamenet::tls::TlsContext::client(cert.ca.string()), {
            .closed = [&](auto) { ++callbacks; }
        }, {}, bad);
        GAMENET_TEST_FAIL("invalid RPC/TLS options accepted");
    } catch (const std::invalid_argument&) {}
    GAMENET_TEST_ASSERT(callbacks == 0 && f.clientConnection->connected());
    f.loop.quit();
    GAMENET_TEST_ASSERT(f.client->start() == TlsOperation::Closed);
    GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::SchedulingRejected});
    f.loop.loop();
}
void callbackAndAbandonment(gamenet::test::TlsCertificates& cert) {
    {
        gamenet::test::RpcTlsFixture f(cert); unsigned done = 0;
        f.serverHandler = [](const auto&, auto) { throw std::runtime_error("contained RPC/TLS handler"); };
        f.onClientClosed = [](auto) { throw std::runtime_error("contained session closed"); };
        f.ready = [&] {
            GAMENET_TEST_ASSERT(f.client->channel()->call(1, "throw", 1s, [&](auto result) {
                GAMENET_TEST_ASSERT(result.status == RpcStatus::RemoteError); ++done;
                (void)f.client->shutdown();
            }).status == RpcOperation::Accepted);
        };
        f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); });
        GAMENET_TEST_ASSERT(done == 1 && f.client->snapshot().callbackExceptions == 1 &&
            f.server->snapshot().rpc.handlerExceptions == 1 && f.client->snapshot().rpc.pendingBytes == 0);
    }
    {
        gamenet::test::RpcTlsFixture f(cert); unsigned done = 0;
        const auto channel = f.client->channel();
        f.ready = [&] {
            GAMENET_TEST_ASSERT(channel->call(1, "abandon", 1s, [&](auto result) {
                GAMENET_TEST_ASSERT(result.status == RpcStatus::Shutdown); ++done;
            }).status == RpcOperation::Accepted);
            f.loop.runAfter(5ms, [&] { f.client.reset(); });
        };
        f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); });
        GAMENET_TEST_ASSERT(done == 1 && f.clientClosed == std::vector{TlsError::Aborted} &&
            channel->snapshot().closed && channel->snapshot().pendingRequests == 0 && channel->snapshot().pendingBytes == 0);
    }
}
void settlementAdmissionFailure(gamenet::test::TlsCertificates& cert) {
    gamenet::net::EventLoopOptions loopOptions;
    loopOptions.maxPendingFunctors = 1; loopOptions.reservedPendingFunctors = 0;
    loopOptions.maxFunctorsPerIteration = 1;
    gamenet::test::RpcTlsFixture f(cert, {}, {}, false, false, 65536, loopOptions);
    f.clientHandler = [&](const auto&, auto) {
        // Saturate the normal queue while inside a borrowed TLS/RPC dispatch.
        // Terminal settlement must reject rather than recurse into buffered work.
        const auto result = f.loop.executor().post([] {});
        GAMENET_TEST_ASSERT(result == gamenet::net::PostResult::Accepted || result == gamenet::net::PostResult::QueueFull);
        (void)f.client->abort();
    };
    f.ready = [&] {
        GAMENET_TEST_ASSERT(f.server->channel()->call(1, "abort in handler", 1s, [](auto) {}).status == RpcOperation::Accepted);
    };
    f.runUntil([&] { return !f.clientClosed.empty(); });
    const auto s = f.client->snapshot();
    GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::SchedulingRejected} && s.settlementRejections == 1 &&
        s.rpc.closed && s.rpc.pendingRequests == 0 && s.rpc.inboundRequests == 0 && s.rpc.pendingBytes == 0);
}
}  // namespace
int main() {
    gamenet::test::TlsCertificates cert;
    auto run = [&](const char* name, auto fn) { std::fprintf(stderr, "RPC-C2: %s\n", name); fn(cert); };
    run("handshake gate/fragmented echo", gateAndEcho); run("certificate/deadline rejection", rejectedHandshake);
    run("request timeout/cancel/pending limits", deadlinesCancelAndPending); run("TLS admission", tlsAdmissionBackpressure);
    run("TCP budget recovery", tcpBackpressureRecovery); run("disconnect/shutdown", disconnectAndExplicitClose);
    run("owner/construction", ownerAndConstruction);
    run("callback exception/abandonment", callbackAndAbandonment);
    run("settlement queue rejection", settlementAdmissionFailure);
}
