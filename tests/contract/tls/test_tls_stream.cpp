// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/TlsFixture.h"
#include "support/TestAssert.h"
#include <thread>
#include <any>
using namespace gamenet::tls;
using namespace std::chrono_literals;
namespace {
void fragmentedEcho(gamenet::test::TlsCertificates& cert) {
    TlsOptions options; options.bioCapacity = 128; options.maxStepsPerDrive = 2;
    gamenet::test::TlsFixture f(cert, options, options); f.fragment = 7; f.forwardWritable = false;
    const std::string payload = std::string("zero\0", 5) + std::string(4096, 'x');
    GAMENET_TEST_ASSERT(f.client->trySend("before TLS ready") == TlsOperation::NotReady);
    GAMENET_TEST_ASSERT(f.client->snapshot().acceptedPlaintextBytes == 0 && f.clientConnection->pendingOutputBytes() == 0);
    f.ready = [&] {
        GAMENET_TEST_ASSERT(f.client->trySend(payload) == TlsOperation::Accepted);
        GAMENET_TEST_ASSERT(f.client->shutdown() == TlsOperation::Accepted);
        GAMENET_TEST_ASSERT(f.client->trySend("late") == TlsOperation::NotReady);
    };
    f.onServerData = [&](auto bytes) { GAMENET_TEST_ASSERT(f.server->trySend(bytes) == TlsOperation::Accepted); };
    f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); });
    GAMENET_TEST_ASSERT(f.clientReady == 1 && f.serverReady == 1 && f.clientData == payload && f.serverData == payload);
    GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::Graceful} && f.serverClosed == std::vector{TlsError::Graceful});
    const auto s = f.client->snapshot();
    GAMENET_TEST_ASSERT(s.sentCloseNotify && s.receivedCloseNotify && s.pendingPlaintextBytes == 0 &&
        s.acceptedPlaintextBytes == payload.size() && s.encryptedPlaintextBytes == payload.size() && s.discardedPlaintextBytes == 0);
    GAMENET_TEST_ASSERT(s.wantRead > 0 && s.wantWrite > 0);
}
void verificationFailures(gamenet::test::TlsCertificates& cert) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        gamenet::test::TlsFixture f(cert, {}, {}, scenario == 0 ? "wrong.local" : "localhost", scenario == 1, scenario == 2);
        f.runUntil([&] { return !f.clientClosed.empty(); });
        GAMENET_TEST_ASSERT(f.clientReady == 0 && f.clientData.empty() && f.clientClosed == std::vector{TlsError::CertificateRejected});
        GAMENET_TEST_ASSERT(f.client->snapshot().verifyResult != 0);
    }
}
void pauseResumeAndBackpressure(gamenet::test::TlsCertificates& cert) {
    TlsOptions options; options.maxCipherInputBytes = 1024 * 1024; options.maxStepsPerDrive = 128;
    gamenet::test::TlsFixture f(cert, options, options, "127.0.0.1", false, false, 8192);
    const std::string payload(256 * 1024, 'p');
    f.forwardWritable = false;
    f.ready = [&] {
        (void)f.server->pauseRead();
        GAMENET_TEST_ASSERT(f.client->trySend(payload) == TlsOperation::Accepted);
        f.loop.runAfter(30ms, [&] {
            GAMENET_TEST_ASSERT(f.serverData.empty());
            (void)f.server->resumeRead();
        });
    };
    f.onServerData = [&](auto) { if (f.serverData.size() == payload.size()) (void)f.client->shutdown(); };
    f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); }, 10s);
    GAMENET_TEST_ASSERT(f.serverData == payload && f.clientClosed == std::vector{TlsError::Graceful});
    GAMENET_TEST_ASSERT(f.client->snapshot().transportRejections > 0);
}
void plaintextLimitsAndReentry(gamenet::test::TlsCertificates& cert) {
    for (bool countLimit : {false, true}) {
        TlsOptions options; options.maxPendingWrites = countLimit ? 1 : 256;
        options.maxPendingPlaintextBytes = countLimit ? 64 : 32;
        gamenet::test::TlsFixture f(cert, options);
        bool tested = false;
        f.ready = [&] { GAMENET_TEST_ASSERT(f.server->trySend("trigger") == TlsOperation::Accepted); };
        f.onClientData = [&](auto) {
            tested = true;
            GAMENET_TEST_ASSERT(f.client->trySend(std::string(32, 'q')) == TlsOperation::Accepted);
            GAMENET_TEST_ASSERT(f.client->trySend("overflow") == TlsOperation::Overloaded);
            gamenet::net::Buffer nested; nested.append(std::string_view("nested"));
            GAMENET_TEST_ASSERT(f.client->onMessage(nested) == TlsOperation::NotReady && nested.readableBytes() == 6);
            (void)f.client->shutdown();
        };
        f.runUntil([&] { return !f.clientClosed.empty() && !f.serverClosed.empty(); });
        GAMENET_TEST_ASSERT(tested && f.serverData == std::string(32, 'q') && f.client->snapshot().reentrantMessages == 1);
    }
}
void deadlinesAndTruncation(gamenet::test::TlsCertificates& cert) {
    {
        TlsOptions options; options.handshakeTimeout = 20ms;
        gamenet::test::TlsFixture f(cert, options);
        f.runUntil([&] { return !f.clientClosed.empty(); }, 2s, false);
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::HandshakeTimeout});
    }
    {
        TlsOptions options; options.shutdownTimeout = 30ms;
        gamenet::test::TlsFixture f(cert, options);
        f.ready = [&] { (void)f.server->pauseRead(); (void)f.client->shutdown(); };
        f.runUntil([&] { return !f.clientClosed.empty(); });
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::ShutdownTimeout});
    }
    {
        gamenet::test::TlsFixture f(cert);
        f.ready = [&] { f.serverConnection->forceClose(); };
        f.runUntil([&] { return !f.clientClosed.empty(); });
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::Truncated});
    }
}
void inputAndOutputLimits(gamenet::test::TlsCertificates& cert) {
    {
        TlsOptions options; options.maxCipherInputBytes = 4096;
        gamenet::test::TlsFixture f(cert, options);
        f.ready = [&] { (void)f.client->pauseRead(); (void)f.server->trySend(std::string(16384, 'o')); };
        f.runUntil([&] { return !f.clientClosed.empty(); });
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::InputLimit});
    }
    {
        TlsOptions options; options.bioCapacity = 128; options.handshakeTimeout = 30ms;
        auto serverOptions = options; serverOptions.handshakeTimeout = 2s;
        // Keep the stalled peer alive: equal deadlines would race peer EOF
        // against the client's timeout and legitimately report Truncated.
        gamenet::test::TlsFixture f(cert, options, serverOptions, "localhost", false, false, 64);
        f.runUntil([&] { return !f.clientClosed.empty(); });
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::HandshakeTimeout});
        const auto s = f.client->snapshot();
        GAMENET_TEST_ASSERT(s.transportRejections > 0 && s.pendingPlaintextBytes == 0 && s.bufferedCipherOutputBytes == 0);
    }
}
void callbackAndOwnerRules(gamenet::test::TlsCertificates& cert) {
    gamenet::test::TlsFixture f(cert);
    TlsOperation foreign{};
    std::thread worker([&] { foreign = f.client->start(); }); worker.join();
    GAMENET_TEST_ASSERT(foreign == TlsOperation::WrongThread);
    f.ready = [&] { GAMENET_TEST_ASSERT(f.server->trySend("throw") == TlsOperation::Accepted); };
    f.onClientData = [&](auto) { throw std::runtime_error("contained TLS callback"); };
    f.runUntil([&] { return !f.clientClosed.empty(); });
    GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::CallbackException} && f.client->snapshot().callbackExceptions == 1);
    (void)f.client->abort(); f.client->onDisconnect();
    GAMENET_TEST_ASSERT(f.clientClosed.size() == 1);
}
void shutdownAdmissionAndContextRelease(gamenet::test::TlsCertificates& cert) {
    {
        gamenet::test::TlsFixture f(cert);
        f.loop.quit(); // a sealed owner cannot accept the handshake deadline
        GAMENET_TEST_ASSERT(f.client->start() == TlsOperation::Closed);
        GAMENET_TEST_ASSERT(f.clientClosed == std::vector{TlsError::SchedulingRejected});
        f.loop.loop();
    }
    {
        gamenet::net::EventLoop loop;
        gamenet::test::ConnectedSocketPair sockets;
        unsigned terminal = 0, down = 0;
        auto connection = gamenet::test::makeTcpConnection(loop, sockets, "tls-context-release");
        connection->setCloseCallback([](const auto& current) { current->connectDestroyed(); });
        connection->setConnectionCallback([&](const auto& current) {
            if (current->connected()) return;
            ++down;
            const auto* stored = std::any_cast<std::shared_ptr<TlsStream>>(&current->getContext());
            if (stored && *stored) { const auto lease = *stored; lease->onDisconnect(); }
            current->setContext(std::any{});
        });
        auto tls = TlsStream::create(connection, TlsContext::client(cert.ca.string()), {
            .data = [](auto) {}, .closed = [&](auto error) { GAMENET_TEST_ASSERT(error == TlsError::Aborted); ++terminal; }
        }, "localhost");
        connection->setContext(tls); connection->connectEstablished();
        (void)tls->start(); tls.reset();
        connection->setContext(std::any{}); // destructor must not re-enter TCP-down here
        GAMENET_TEST_ASSERT(terminal == 1 && down == 0);
        connection->forceClose(); loop.quit(); loop.loop();
        GAMENET_TEST_ASSERT(terminal == 1 && down == 1);
    }
}
}  // namespace
int main() {
    gamenet::test::TlsCertificates cert;
    auto run = [&](const char* name, auto function) { std::fprintf(stderr, "TLS scenario: %s\n", name); function(cert); };
    run("fragmented echo", fragmentedEcho); run("verification failures", verificationFailures);
    run("pause/resume/backpressure", pauseResumeAndBackpressure); run("plaintext limits/reentry", plaintextLimitsAndReentry);
    run("deadlines/truncation", deadlinesAndTruncation); run("input/output limits", inputAndOutputLimits);
    run("callback/owner", callbackAndOwnerRules);
    run("sealed owner/context release", shutdownAdmissionAndContextRelease);
}
