// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/rpc_tls/RpcTlsSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/TlsCertificates.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include <vector>
#include <cstdio>

namespace gamenet::test {
struct RpcTlsFixture {
    net::EventLoop loop;
    ConnectedSocketPair sockets;
    net::TcpConnectionPtr clientConnection, serverConnection;
    std::shared_ptr<rpc::RpcTlsSession> client, server;
    unsigned clientReady{}, serverReady{};
    std::vector<tls::TlsError> clientClosed, serverClosed;
    rpc::RpcChannel::Handler clientHandler, serverHandler;
    std::function<void()> ready;
    std::function<void(tls::TlsError)> onClientClosed;
    bool readyCalled{}, forwardWritable{true};
    std::size_t fragment{};

    RpcTlsFixture(TlsCertificates& cert, rpc::RpcTlsOptions clientOptions = {},
        rpc::RpcTlsOptions serverOptions = {}, bool wrongCa = false, bool expired = false,
        std::size_t sendLimit = 65536, net::EventLoopOptions loopOptions = {}) : loop(loopOptions) {
        clientConnection = makeTcpConnection(loop, sockets, "rpc-tls-client");
        std::swap(sockets.connectionFd, sockets.peerFd);
        serverConnection = makeTcpConnection(loop, sockets, "rpc-tls-server");
        sockets.peerFd = net::kInvalidSocket;
        for (const auto& connection : {clientConnection, serverConnection}) {
            connection->setSendBufferSize(4096);
            connection->setBackpressureOptions({.lowWaterMarkBytes = sendLimit / 4,
                .highWaterMarkBytes = sendLimit / 2, .hardLimitBytes = sendLimit});
            connection->setCloseCallback([](const auto& c) { c->connectDestroyed(); });
            connection->setCallbackExceptionHandler([](const auto&, const auto&) {
                GAMENET_TEST_FAIL("RPC/TLS callback escaped into Core");
            });
        }
        if (clientOptions.peerName.empty()) clientOptions.peerName = "localhost";
        client = rpc::RpcTlsSession::create(clientConnection,
            tls::TlsContext::client((wrongCa ? cert.unrelatedCa : cert.ca).string()), {
                .ready = [&] { ++clientReady; runReady(); },
                .closed = [&](auto error) { clientClosed.push_back(error); if (onClientClosed) onClientClosed(error); }
            }, [&](const auto& request, auto reply) { if (clientHandler) clientHandler(request, std::move(reply)); }, clientOptions);
        server = rpc::RpcTlsSession::create(serverConnection,
            tls::TlsContext::server((expired ? cert.expiredCertificate : cert.certificate).string(), cert.key.string()), {
                .ready = [&] { ++serverReady; runReady(); },
                .closed = [&](auto error) { serverClosed.push_back(error); }
            }, [&](const auto& request, auto reply) { if (serverHandler) serverHandler(request, std::move(reply)); }, serverOptions);
        clientConnection->setConnectionCallback([&](const auto& c) { if (!c->connected() && client) client->onDisconnect(); });
        serverConnection->setConnectionCallback([&](const auto& c) { if (!c->connected() && server) server->onDisconnect(); });
        clientConnection->setMessageCallback([&](const auto&, auto* input) { deliver(client, *input); });
        serverConnection->setMessageCallback([&](const auto&, auto* input) { deliver(server, *input); });
        clientConnection->setWriteCompleteCallback([&](const auto&) { if (forwardWritable && client) client->onWritable(); });
        serverConnection->setWriteCompleteCallback([&](const auto&) { if (forwardWritable && server) server->onWritable(); });
        clientConnection->connectEstablished(); serverConnection->connectEstablished();
    }
    void runReady() { if (clientReady && serverReady && !readyCalled) { readyCalled = true; if (ready) ready(); } }
    void deliver(std::shared_ptr<rpc::RpcTlsSession> session, net::Buffer& input) {
        if (!session) { input.retrieveAll(); return; }
        if (!fragment) { (void)session->onMessage(input); return; }
        while (input.readableBytes()) {
            const auto count = std::min(fragment, input.readableBytes());
            net::Buffer piece; piece.append(std::string_view(input.peek(), count)); input.retrieve(count);
            (void)session->onMessage(piece);
        }
    }
    void cleanup() {
        if (client) (void)client->abort(); if (server) (void)server->abort();
        clientConnection->forceClose(); serverConnection->forceClose();
    }
    void runUntil(std::function<bool()> done, bool startServer = true) {
        if (startServer) (void)server->start();
        (void)client->start();
        loop.runEvery(std::chrono::milliseconds(1), [&, done] { if (done()) { cleanup(); loop.quit(); } });
        loop.runAfter(std::chrono::seconds(10), [&] {
            const auto s = client ? client->snapshot() : rpc::RpcTlsSnapshot{};
            std::fprintf(stderr, "RPC/TLS timeout phase=%d TLS=%d pending=%zu closed=%zu/%zu\n",
                static_cast<int>(s.phase), static_cast<int>(s.tls.phase), s.rpc.pendingRequests,
                clientClosed.size(), serverClosed.size());
            GAMENET_TEST_FAIL("RPC/TLS fixture timed out");
        });
        loop.loop();
    }
    ~RpcTlsFixture() {
        cleanup();
        if (loop.phase() == net::EventLoopPhase::Running) { loop.quit(); loop.loop(); }
        client.reset(); server.reset();
    }
};
}  // namespace gamenet::test
