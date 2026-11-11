// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/tls/TlsStream.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TlsCertificates.h"
#include <functional>
#include <vector>
#include <cstdio>

namespace gamenet::test {
struct TlsFixture {
    net::EventLoop loop;
    ConnectedSocketPair sockets;
    net::TcpConnectionPtr clientConnection, serverConnection;
    std::shared_ptr<tls::TlsStream> client, server;
    unsigned clientReady{}, serverReady{};
    std::vector<tls::TlsError> clientClosed, serverClosed;
    std::string clientData, serverData;
    std::function<void()> ready;
    std::function<void(std::string_view)> onClientData, onServerData;
    std::size_t fragment{};
    bool forwardWritable{true}, readyCalled{};

    TlsFixture(TlsCertificates& cert, tls::TlsOptions clientOptions = {}, tls::TlsOptions serverOptions = {},
        std::string name = "localhost", bool wrongCa = false, bool expired = false,
        std::size_t hardLimit = 65536, net::EventLoopOptions loopOptions = {}) : loop(loopOptions) {
        clientConnection = makeTcpConnection(loop, sockets, "tls-client");
        std::swap(sockets.connectionFd, sockets.peerFd);
        serverConnection = makeTcpConnection(loop, sockets, "tls-server");
        sockets.peerFd = net::kInvalidSocket; // both descriptors now owned by connections
        for (const auto& connection : {clientConnection, serverConnection}) {
            connection->setSendBufferSize(4096);
            connection->setBackpressureOptions({.lowWaterMarkBytes = hardLimit / 4,
                .highWaterMarkBytes = hardLimit / 2, .hardLimitBytes = hardLimit});
            connection->setCloseCallback([](const auto& current) { current->connectDestroyed(); });
            connection->setCallbackExceptionHandler([](const auto&, const auto&) {
                GAMENET_TEST_FAIL("TLS callback exception escaped into Core");
            });
        }
        client = tls::TlsStream::create(clientConnection,
            tls::TlsContext::client((wrongCa ? cert.unrelatedCa : cert.ca).string()), {
                .ready = [&] { ++clientReady; runReady(); },
                .data = [&](auto bytes) { clientData.append(bytes); if (onClientData) onClientData(bytes); },
                .closed = [&](auto error) { clientClosed.push_back(error); }
            }, std::move(name), clientOptions);
        server = tls::TlsStream::create(serverConnection,
            tls::TlsContext::server((expired ? cert.expiredCertificate : cert.certificate).string(), cert.key.string()), {
                .ready = [&] { ++serverReady; runReady(); },
                .data = [&](auto bytes) { serverData.append(bytes); if (onServerData) onServerData(bytes); },
                .closed = [&](auto error) { serverClosed.push_back(error); }
            }, {}, serverOptions);
        clientConnection->setMessageCallback([&](const auto&, auto* bytes) { deliver(client, *bytes); });
        serverConnection->setMessageCallback([&](const auto&, auto* bytes) { deliver(server, *bytes); });
        clientConnection->setConnectionCallback([&](const auto& c) { if (!c->connected() && client) client->onDisconnect(); });
        serverConnection->setConnectionCallback([&](const auto& c) { if (!c->connected() && server) server->onDisconnect(); });
        clientConnection->setWriteCompleteCallback([&](const auto&) { if (forwardWritable && client) client->onWritable(); });
        serverConnection->setWriteCompleteCallback([&](const auto&) { if (forwardWritable && server) server->onWritable(); });
        clientConnection->connectEstablished(); serverConnection->connectEstablished();
    }
    void runReady() {
        if (clientReady && serverReady && ready && !readyCalled) { readyCalled = true; ready(); }
    }
    void deliver(std::shared_ptr<tls::TlsStream> stream, net::Buffer& bytes) {
        if (!stream) { bytes.retrieveAll(); return; }
        if (!fragment) { (void)stream->onMessage(bytes); return; }
        while (bytes.readableBytes()) {
            const auto count = std::min(fragment, bytes.readableBytes());
            net::Buffer part; part.append(std::string_view(bytes.peek(), count)); bytes.retrieve(count);
            (void)stream->onMessage(part);
        }
    }
    void cleanup() {
        if (client) (void)client->abort(); if (server) (void)server->abort();
        clientConnection->forceClose(); serverConnection->forceClose();
    }
    void runUntil(std::function<bool()> done, std::chrono::milliseconds timeout = std::chrono::seconds(5),
        bool startServer = true) {
        if (startServer) (void)server->start();
        (void)client->start();
        loop.runEvery(std::chrono::milliseconds(1), [&, done] {
            if (done()) { cleanup(); loop.quit(); }
        });
        loop.runAfter(timeout, [&] {
            for (const auto& value : {client, server}) if (value) {
                const auto s = value->snapshot();
                std::fprintf(stderr, "TLS timeout phase=%d terminal=%d plain=%zu cipherIn=%zu cipherOut=%zu sent=%d recv=%d rejected=%llu\n",
                    static_cast<int>(s.phase), static_cast<int>(s.terminal), s.pendingPlaintextBytes,
                    s.bufferedCipherInputBytes, s.bufferedCipherOutputBytes, s.sentCloseNotify, s.receivedCloseNotify,
                    static_cast<unsigned long long>(s.transportRejections));
            }
            GAMENET_TEST_FAIL("TLS fixture timed out");
        });
        loop.loop();
    }
    ~TlsFixture() {
        cleanup();
        if (loop.phase() == net::EventLoopPhase::Running) { loop.quit(); loop.loop(); }
        client.reset(); server.reset();
    }
};
}  // namespace gamenet::test
