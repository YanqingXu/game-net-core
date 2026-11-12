// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket_tls/WebSocketTlsSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TlsCertificates.h"
#include "support/WebSocketWire.h"
#include <vector>
namespace gamenet::test {
struct WebSocketTlsFixture {
    net::EventLoop loop;
    ConnectedSocketPair sockets;
    net::TcpConnectionPtr clientConnection,serverConnection;
    std::shared_ptr<tls::TlsStream> client;
    std::shared_ptr<websocket::WebSocketTlsSession> server;
    unsigned clientReady{},serverReady{},messages{},policies{};
    std::vector<tls::TlsError> clientClosed;
    std::vector<websocket::WebSocketTlsResult> serverClosed;
    std::string wire;
    std::function<void()> ready;
    std::function<void(std::string_view)> clientData;
    std::function<void(websocket::WebSocketOpcode,std::string_view)> message;
    std::function<void(websocket::WebSocketTlsResult)> closed;
    std::function<bool(const websocket::WebSocketRequest&)> policy;
    bool autoUpgrade{true},forwardWritable{true},releaseOnDown{};
    std::string upgrade{websocketUpgrade};
    explicit WebSocketTlsFixture(TlsCertificates& cert,websocket::WebSocketTlsOptions options={},
        tls::TlsOptions clientOptions={},std::string peer="localhost",bool wrongCa=false,bool expired=false,
        std::size_t hardLimit=65536,net::EventLoopOptions loops={}) :loop(loops) {
        clientConnection=makeTcpConnection(loop,sockets,"wss-wire-client");
        std::swap(sockets.connectionFd,sockets.peerFd);
        serverConnection=makeTcpConnection(loop,sockets,"wss-server");sockets.peerFd=net::kInvalidSocket;
        for(const auto& c:{clientConnection,serverConnection}) {
            c->setSendBufferSize(4096);
            c->setBackpressureOptions({.lowWaterMarkBytes=hardLimit/4,.highWaterMarkBytes=hardLimit/2,.hardLimitBytes=hardLimit});
            c->setCloseCallback([](const auto& c){c->connectDestroyed();});
            c->setCallbackExceptionHandler([](const auto&,const auto&){GAMENET_TEST_FAIL("WSS escaped into Core");});
        }
        client=tls::TlsStream::create(clientConnection,tls::TlsContext::client((wrongCa?cert.unrelatedCa:cert.ca).string()),{
            .ready=[&]{++clientReady;if(autoUpgrade)GAMENET_TEST_ASSERT(client->trySend(upgrade)==tls::TlsOperation::Accepted);},
            .data=[&](auto bytes){wire.append(bytes);if(clientData)clientData(bytes);},
            .closed=[&](auto error){clientClosed.push_back(error);}
        },std::move(peer),clientOptions);
        server=websocket::WebSocketTlsSession::create(serverConnection,
            tls::TlsContext::server((expired?cert.expiredCertificate:cert.certificate).string(),cert.key.string()),{
                .handshake=[&](const auto& request){++policies;return !policy || policy(request);},
                .ready=[&]{++serverReady;if(ready)ready();},
                .message=[&](auto type,auto bytes){++messages;if(message)message(type,bytes);},
                .closed=[&](auto error){serverClosed.push_back(error);if(closed)closed(error);}
            },options);
        clientConnection->setConnectionCallback([&](const auto& c){if(!c->connected() && client)client->onDisconnect();});
        serverConnection->setConnectionCallback([&](const auto& c){if(!c->connected() && server){server->onDisconnect();if(releaseOnDown)server.reset();}});
        clientConnection->setMessageCallback([&](const auto&,auto* b){(void)client->onMessage(*b);});
        serverConnection->setMessageCallback([&](const auto&,auto* b){if(server)(void)server->onMessage(*b);else b->retrieveAll();});
        clientConnection->setWriteCompleteCallback([&](const auto&){if(forwardWritable && client)client->onWritable();});
        serverConnection->setWriteCompleteCallback([&](const auto&){if(forwardWritable && server)server->onWritable();});
        clientConnection->connectEstablished();serverConnection->connectEstablished();
    }
    void cleanup(){if(server)(void)server->abort();if(client)(void)client->abort();clientConnection->forceClose();serverConnection->forceClose();}
    void run(std::function<bool()> done,bool startClient=true) {
        (void)server->start();if(startClient)(void)client->start();
        loop.runEvery(std::chrono::milliseconds(1),[&,done]{if(done()){cleanup();loop.quit();}});
        loop.runAfter(std::chrono::seconds(8),[&]{
            if(server){const auto s=server->snapshot();std::fprintf(stderr,"WSS timeout phase=%d ws=%d tls=%d closed=%zu/%zu wire=%zu pending=%zu/%zu\n",
                int(s.phase),int(s.websocket.phase),int(s.tls.phase),serverClosed.size(),clientClosed.size(),wire.size(),s.websocket.pendingControlBytes,s.tls.pendingPlaintextBytes);}
            GAMENET_TEST_FAIL("WSS fixture timed out");
        });loop.loop();
    }
    void settled() {
        const auto s=server->snapshot();
        GAMENET_TEST_ASSERT(s.phase==websocket::WebSocketTlsPhase::Closed && serverClosed.size()==1 &&
            !s.websocket.inputBytes && !s.websocket.fragmentedBytes && !s.websocket.pendingControls && !s.websocket.pendingControlBytes &&
            !s.websocket.continuationQueued && !s.tls.pendingPlaintextBytes && !s.tls.pendingWrites &&
            !s.tls.bufferedCipherInputBytes && !s.tls.bufferedCipherOutputBytes &&
            s.websocket.enqueuedControlBytes==s.websocket.admittedControlBytes+s.websocket.discardedControlBytes &&
            s.tls.acceptedPlaintextBytes==s.tls.encryptedPlaintextBytes+s.tls.discardedPlaintextBytes);
    }
    ~WebSocketTlsFixture(){cleanup();if(loop.phase()==net::EventLoopPhase::Running){loop.quit();loop.loop();}}
};
} // namespace gamenet::test
