// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/TestAssert.h"
#include "support/WebSocketWire.h"
#include <any>
#include <array>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

namespace gamenet::test {
struct WsStressServer {
    unsigned generation{};
    std::shared_ptr<void> lifetime;
    std::function<void()> start,down,writable,settled,bounded;
    std::function<void(net::Buffer&)> feed;
    std::function<websocket::WebSocketOperation(websocket::WebSocketOpcode,std::string_view)> send;
    std::function<websocket::WebSocketOperation()> close,abort;
    std::function<websocket::WebSocketSnapshot()> snapshot;
};
struct WsStressClient {
    std::shared_ptr<void> lifetime;
    std::function<void()> start,down,writable,shutdown,settled,bounded;
    std::function<void(net::Buffer&)> feed;
    std::function<bool(std::string_view)> send;
};
inline std::shared_ptr<WsStressServer> plainWsStressServer(const net::TcpConnectionPtr& c,websocket::WebSocketCallbacks callbacks) {
    auto out=std::make_shared<WsStressServer>();
    auto s=websocket::WebSocketSession::create(c,std::move(callbacks));out->lifetime=s;
    out->start=[s]{(void)s->start();};out->down=[s]{s->onDisconnect();};out->writable=[s]{s->onWritable();};
    out->feed=[s](auto& b){(void)s->onMessage(b);};out->send=[s](auto type,auto b){return s->send(type,b);};
    out->close=[s]{return s->close();};out->abort=[s]{return s->abort();};out->snapshot=[s]{return s->snapshot();};out->settled=[]{};out->bounded=[]{};
    return out;
}
inline std::shared_ptr<WsStressClient> plainWsStressClient(const net::TcpConnectionPtr& c,
    std::function<void()> ready,std::function<void(std::string_view)> data,std::function<void(bool)> closed) {
    auto out=std::make_shared<WsStressClient>();const std::weak_ptr<net::TcpConnection> weak=c;
    out->start=std::move(ready);out->feed=[data=std::move(data)](auto& b){const std::string owned(b.peek(),b.readableBytes());b.retrieveAll();data(owned);};
    out->send=[weak](auto b){const auto c=weak.lock();return c && c->trySend(b)==net::TcpSendResult::Accepted;};
    out->down=[weak,closed=std::move(closed)]{const auto c=weak.lock();const auto info=c?c->closeInfo():std::nullopt;
        closed(info && info->nativeError==0 && (info->reason==net::TcpConnectionCloseReason::PeerEof || info->reason==net::TcpConnectionCloseReason::GracefulShutdown));};
    out->shutdown=[weak]{if(auto c=weak.lock())c->shutdown();};out->writable=[]{};out->settled=[]{};out->bounded=[]{};return out;
}

// Real loopback TCP in every mode. Optional factories compose real TLS; only
// test-owned input delivery is held, with a finite cap. No network is mocked.
template<class ServerFactory,class ClientFactory>
void runWebSocketReconnectStress(ServerFactory serverFactory,ClientFactory clientFactory,const char* label) {
    using namespace std::chrono_literals;
    using namespace websocket;
    constexpr unsigned generations=8,burst=128;
    struct Generation {
        unsigned serverReady{},clientReady{},serverDown{},clientDown{},serverClosed{},clientClosed{},requests{},pongs{},received{},tailReceived{};
        unsigned accepted{},rejected{},tailAccepted{};
        bool probe{},closeSeen{},serverGraceful{},clientGraceful{},requestSent{},staleChecked{};
        std::vector<std::string> expected,tail;
        std::string input;
        std::uint64_t inputBytes{},consumed{},discarded{};
    };
    std::array<Generation,generations> ledger{};
    net::EventLoop loop;
    net::TcpServer server(&loop,net::InetAddress(0,true),"ws-c3-stress-server");
    server.setConnectionBackpressureOptions({.lowWaterMarkBytes=2048,.highWaterMarkBytes=4096,.hardLimitBytes=8192});
    enum class Phase { Connect,Hold,Recover,Probe,Drain,Retire };
    Phase phase=Phase::Connect;unsigned generation=0,totalRejected=0;
    bool hold=false,staleReleased=true;
    net::Buffer held;
    std::shared_ptr<WsStressServer> serverSession,stale;
    std::shared_ptr<WsStressClient> clientSession;
    net::TcpConnectionPtr serverConnection,clientConnection;
    std::unique_ptr<net::TcpClient> client;
    std::vector<std::weak_ptr<void>> retired;
    auto serverContext=[](const auto& c){const auto* p=std::any_cast<std::shared_ptr<WsStressServer>>(&c->getContext());return p?*p:nullptr;};
    auto clientContext=[](const auto& c){const auto* p=std::any_cast<std::shared_ptr<WsStressClient>>(&c->getContext());return p?*p:nullptr;};
    auto data=[&](unsigned id,std::string_view bytes){
        auto& g=ledger[id];GAMENET_TEST_ASSERT(!g.clientClosed && id==generation);
        g.inputBytes+=bytes.size();g.input.append(bytes);GAMENET_TEST_ASSERT(g.input.size()<=1024*1024);
        if(!g.clientReady){
            const auto end=g.input.find("\r\n\r\n");if(end==std::string::npos)return;
            GAMENET_TEST_ASSERT(g.input.starts_with("HTTP/1.1 101") && g.input.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")!=std::string::npos);
            g.input.erase(0,end+4);g.consumed+=end+4;++g.clientReady;
        }
        while(g.input.size()>=2){
            const auto first=static_cast<unsigned char>(g.input[0]),second=static_cast<unsigned char>(g.input[1]);
            GAMENET_TEST_ASSERT((first&0xf0)==0x80 && !(second&0x80));
            std::size_t header=2;std::uint64_t length=second&127;
            if(length>=126){const unsigned n=length==126?2:8;if(g.input.size()<2+n)return;length=0;
                for(unsigned k=0;k<n;++k)length=(length<<8)|static_cast<unsigned char>(g.input[2+k]);header+=n;}
            GAMENET_TEST_ASSERT(length<=8192);if(g.input.size()<header+length)return;
            const auto op=first&15;const std::string payload=g.input.substr(header,static_cast<std::size_t>(length));
            g.input.erase(0,header+static_cast<std::size_t>(length));g.consumed+=header+length;
            if(op==2){
                if(g.received<g.expected.size())GAMENET_TEST_ASSERT(payload==g.expected[g.received++]);
                else {GAMENET_TEST_ASSERT(g.tailReceived<g.tail.size() && payload==g.tail[g.tailReceived]);++g.tailReceived;}
            }else if(op==1){GAMENET_TEST_ASSERT(payload=="probe-"+std::to_string(id) && !g.probe);g.probe=true;}
            else if(op==10){GAMENET_TEST_ASSERT(payload=="stress" && ++g.pongs==1);}
            else if(op==8){
                GAMENET_TEST_ASSERT(id%2==0 && !g.closeSeen && payload==std::string("\x03\xe8",2));g.closeSeen=true;
                GAMENET_TEST_ASSERT(clientSession->send(websocketClientFrame(8,payload)));clientSession->shutdown();
            }else GAMENET_TEST_FAIL("unexpected stress frame opcode");
        }
    };
    server.setMessageCallback([&](const auto& c,auto* b){if(auto s=serverContext(c))s->feed(*b);else b->retrieveAll();});
    server.setWriteCompleteCallback([&](const auto& c){if(auto s=serverContext(c))s->writable();});
    server.setConnectionCallback([&](const auto& c){
        if(!c->connected()){const auto s=serverContext(c);GAMENET_TEST_ASSERT(s);const auto id=s->generation;
            s->down();c->setContext(std::any{});GAMENET_TEST_ASSERT(++ledger[id].serverDown==1);return;}
        c->setSendBufferSize(4096);serverConnection=c;const auto id=generation;
        serverSession=serverFactory(c,WebSocketCallbacks{
            .ready=[&,id]{GAMENET_TEST_ASSERT(id==generation && ++ledger[id].serverReady==1);},
            .message=[&,id](auto type,auto bytes){GAMENET_TEST_ASSERT(id==generation && !ledger[id].serverClosed &&
                type==WebSocketOpcode::Text && bytes=="request-"+std::to_string(id) && ++ledger[id].requests==1);},
            .closed=[&,id](auto error){GAMENET_TEST_ASSERT(++ledger[id].serverClosed==1);ledger[id].serverGraceful=error==WebSocketError::Graceful;}
        });serverSession->generation=id;c->setContext(serverSession);serverSession->start();
    });
    server.start();
    auto connect=[&]{
        const auto id=generation;
        client=std::make_unique<net::TcpClient>(&loop,server.listenAddress(),"ws-c3-wire-client");
        client->setMessageCallback([&,id](const auto& c,auto* b){
            GAMENET_TEST_ASSERT(id==generation && !ledger[id].clientClosed);
            if(hold){GAMENET_TEST_ASSERT(held.readableBytes()+b->readableBytes()<=1024*1024);held.append(std::string_view(b->peek(),b->readableBytes()));b->retrieveAll();}
            else if(auto s=clientContext(c))s->feed(*b);else b->retrieveAll();
        });
        client->setWriteCompleteCallback([&](const auto& c){if(auto s=clientContext(c))s->writable();});
        client->setConnectionCallback([&,id](const auto& c){
            if(!c->connected()){
                if(auto s=clientContext(c)){if(held.readableBytes())s->feed(held);s->down();}
                c->setContext(std::any{});GAMENET_TEST_ASSERT(++ledger[id].clientDown==1);return;
            }
            clientConnection=c;GAMENET_TEST_ASSERT(id==generation);
            clientSession=clientFactory(c,[&,id]{GAMENET_TEST_ASSERT(id==generation);GAMENET_TEST_ASSERT(clientSession->send(websocketUpgrade));},
                [&,id](auto bytes){data(id,bytes);},[&,id](bool graceful){auto& g=ledger[id];GAMENET_TEST_ASSERT(++g.clientClosed==1);
                    // A clean TCP EOF alone is not a completed WS close.
                    g.clientGraceful=graceful && g.closeSeen;g.discarded+=g.input.size();g.input.clear();});
            c->setContext(clientSession);clientSession->start();
        });client->connect();
    };
    auto bounds=[&]{if(!serverSession)return;serverSession->bounded();if(clientSession)clientSession->bounded();const auto s=serverSession->snapshot();
        GAMENET_TEST_ASSERT(s.pendingControlBytes<=1024 && s.enqueuedControlBytes==s.admittedControlBytes+s.discardedControlBytes+s.pendingControlBytes &&
            serverConnection->pendingOutputBytes()<=8192);};
    connect();
    loop.runEvery(1ms,[&]{
        auto& g=ledger[generation];bounds();
        switch(phase){
        case Phase::Connect:
            if(!g.serverReady || !g.clientReady)break;
            if(!g.requestSent){g.requestSent=true;const auto request="request-"+std::to_string(generation);
                GAMENET_TEST_ASSERT(clientSession->send(websocketClientFrame(1,request.substr(0,3),false)+websocketClientFrame(9,"stress")+websocketClientFrame(0,request.substr(3))));break;}
            if(!g.requests || !g.pongs)break;
            if(stale){const auto before=serverSession->snapshot();
                net::Buffer old;old.append(websocketClientFrame(1,"old-generation"));stale->feed(old);stale->down();stale->writable();
                GAMENET_TEST_ASSERT(stale->send(WebSocketOpcode::Text,"stale")==WebSocketOperation::Closed);
                (void)stale->close();(void)stale->abort();
                GAMENET_TEST_ASSERT(serverSession->snapshot().receivedMessages==before.receivedMessages && ledger[generation-1].serverClosed==1);
                stale.reset();staleReleased=true;
            }
            for(const auto& weak:retired)if(!weak.expired())return;retired.clear();
            g.staleChecked=true;hold=true;
            for(unsigned n=0;n<burst;++n){const auto payload=std::to_string(generation)+":"+std::to_string(n)+":"+std::string(4096,'x');
                const auto result=serverSession->send(WebSocketOpcode::Binary,payload);
                if(result==WebSocketOperation::Accepted){g.expected.push_back(payload);++g.accepted;}
                else {GAMENET_TEST_ASSERT(result==WebSocketOperation::Overloaded);++g.rejected;}bounds();}
            GAMENET_TEST_ASSERT(g.accepted && g.rejected);totalRejected+=g.rejected;phase=Phase::Hold;break;
        case Phase::Hold:
            if(!held.readableBytes())break;hold=false;phase=Phase::Recover;clientSession->feed(held);GAMENET_TEST_ASSERT(!held.readableBytes());break;
        case Phase::Recover:
            if(g.received!=g.accepted || serverConnection->pendingOutputBytes())break;
            GAMENET_TEST_ASSERT(serverSession->send(WebSocketOpcode::Text,"probe-"+std::to_string(generation))==WebSocketOperation::Accepted);phase=Phase::Probe;break;
        case Phase::Probe:
            if(!g.probe)break;phase=Phase::Drain;
            if(generation%2==0)GAMENET_TEST_ASSERT(serverSession->close()==WebSocketOperation::Accepted);
            else {
                hold=true;unsigned rejected=0;
                for(unsigned n=0;n<burst;++n){const auto payload="tail-"+std::to_string(generation)+":"+std::to_string(n)+":"+std::string(4096,'t');
                    const auto result=serverSession->send(WebSocketOpcode::Binary,payload);
                    if(result==WebSocketOperation::Accepted){g.tail.push_back(payload);++g.tailAccepted;}else {GAMENET_TEST_ASSERT(result==WebSocketOperation::Overloaded);++rejected;}bounds();}
                GAMENET_TEST_ASSERT(g.tailAccepted && rejected && serverConnection->pendingOutputBytes()>0);serverConnection->forceClose();
            }break;
        case Phase::Drain:
            if(!g.serverDown || !g.clientDown || !g.serverClosed || !g.clientClosed)break;
            GAMENET_TEST_ASSERT(g.serverDown==1 && g.clientDown==1 && g.serverClosed==1 && g.clientClosed==1 && g.probe && g.staleChecked &&
                g.received==g.accepted && g.inputBytes==g.consumed+g.discarded && !g.input.size() && !held.readableBytes());
            GAMENET_TEST_ASSERT(g.serverGraceful==(generation%2==0) && g.clientGraceful==(generation%2==0));
            {const auto s=serverSession->snapshot();GAMENET_TEST_ASSERT(s.phase==WebSocketPhase::Closed && !s.inputBytes && !s.fragmentedBytes && !s.pendingControls && !s.pendingControlBytes && !s.continuationQueued &&
                s.receivedMessages==1 && s.acceptedSends==g.accepted+g.tailAccepted+1);}
            serverSession->settled();clientSession->settled();
            GAMENET_TEST_ASSERT(!serverConnection->pendingOutputBytes() && !clientConnection->pendingOutputBytes());
            retired={serverSession,clientSession,serverConnection,clientConnection};
            if(serverSession->lifetime)retired.push_back(serverSession->lifetime);if(clientSession->lifetime)retired.push_back(clientSession->lifetime);
            stale=serverSession;staleReleased=false;serverSession.reset();clientSession.reset();serverConnection.reset();clientConnection.reset();client->stop();client.reset();
            hold=false;phase=Phase::Retire;break;
        case Phase::Retire:
            if(generation+1==generations){if(!staleReleased){stale->down();stale->writable();(void)stale->abort();stale.reset();staleReleased=true;}
                for(const auto& weak:retired)if(!weak.expired())return;server.stop();loop.quit();break;}
            ++generation;phase=Phase::Connect;connect();break;
        }
    });
    loop.runAfter(15s,[&]{std::fprintf(stderr,"%s stress timeout generation=%u phase=%d\n",label,generation,int(phase));GAMENET_TEST_FAIL("WS stress timeout");});
    loop.loop();GAMENET_TEST_ASSERT(generation==generations-1 && totalRejected>=generations && !stale);
    for(const auto& weak:retired)GAMENET_TEST_ASSERT(weak.expired());
}
} // namespace gamenet::test
