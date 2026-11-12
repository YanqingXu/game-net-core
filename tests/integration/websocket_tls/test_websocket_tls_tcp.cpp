// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket_tls/WebSocketTlsSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/TestAssert.h"
#include "support/WebSocketWire.h"
#include "support/TlsCertificates.h"
#include "support/WebSocketReconnectStress.h"
#include <any>
#include <vector>
using namespace gamenet::websocket;
using namespace std::chrono_literals;
int main(int argc,char** argv){
    gamenet::test::TlsCertificates cert;
    if(argc==2){std::filesystem::create_directories(argv[1]);for(const auto& p:{cert.ca,cert.certificate,cert.key,cert.expiredCertificate,cert.unrelatedCa})std::filesystem::copy_file(p,std::filesystem::path(argv[1])/p.filename(),std::filesystem::copy_options::overwrite_existing);return 0;}
    for(unsigned generation=0;generation<8;++generation){
        gamenet::net::EventLoop loop;unsigned ready=0,messages=0,closed=0;bool down=false;
        std::weak_ptr<WebSocketTlsSession> weak;
        gamenet::net::TcpServer server(&loop,gamenet::net::InetAddress(0,true),"ws-integration");
        server.setConnectionCallback([&](const auto& c){
            if(!c->connected()){auto s=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(c->getContext());s->onDisconnect();c->setContext(std::any{});return;}
            auto s=WebSocketTlsSession::create(c,gamenet::tls::TlsContext::server(cert.certificate.string(),cert.key.string()),{
                .ready=[&]{++ready;},
                .message=[&,weakConnection=std::weak_ptr(c)](auto type,auto payload){++messages;auto connection=weakConnection.lock();
                    auto session=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(connection->getContext());
                    GAMENET_TEST_ASSERT(session->send(type,payload)==WebSocketOperation::Accepted);},
                .closed=[&](auto error){GAMENET_TEST_ASSERT(error.graceful());++closed;}
            });weak=s;c->setContext(s);(void)s->start();
        });
        server.setMessageCallback([](const auto& c,auto* b){auto s=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(c->getContext());(void)s->onMessage(*b);});
        server.start();gamenet::net::TcpClient client(&loop,server.listenAddress(),"independent-wire-client");
        std::string received;bool sent=false,closeSent=false;
        std::shared_ptr<gamenet::tls::TlsStream> tlsClient;
        const auto payload=std::string("binary\0",7)+std::string(4096,'x');
        auto receive=[&](std::string_view bytes){received.append(bytes);
            if(!sent && received.find("\r\n\r\n")!=std::string::npos){
                GAMENET_TEST_ASSERT(received.starts_with("HTTP/1.1 101") && received.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")!=std::string::npos);
                received.erase(0,received.find("\r\n\r\n")+4);sent=true;
                tlsClient->trySend(gamenet::test::websocketClientFrame(2,std::string_view(payload).substr(0,3),false)+
                    gamenet::test::websocketClientFrame(9,"ping")+gamenet::test::websocketClientFrame(0,std::string_view(payload).substr(3)));
            }
            const auto expected=*WebSocketCodec::encodeServer(WebSocketOpcode::Pong,"ping")+*WebSocketCodec::encodeServer(WebSocketOpcode::Binary,payload);
            if(sent && !closeSent && received.size()>=expected.size()){
                GAMENET_TEST_ASSERT(received==expected);received.clear();closeSent=true;
                tlsClient->trySend(gamenet::test::websocketClientFrame(8,std::string("\x03\xe8",2)));
            }
            if(closeSent && received.size()>=4){GAMENET_TEST_ASSERT(received==std::string("\x88\x02\x03\xe8",4));(void)tlsClient->shutdown();}
        };
        client.setConnectionCallback([&](const auto& c){if(c->connected()){
            tlsClient=gamenet::tls::TlsStream::create(c,gamenet::tls::TlsContext::client(cert.ca.string()),{
                .ready=[&]{(void)tlsClient->trySend(std::string_view(gamenet::test::websocketUpgrade).substr(0,7));
                    loop.runAfter(1ms,[&]{(void)tlsClient->trySend(std::string_view(gamenet::test::websocketUpgrade).substr(7));});},
                .data=receive,.closed=[](auto error){GAMENET_TEST_ASSERT(error==gamenet::tls::TlsError::Graceful);}
            },"localhost");(void)tlsClient->start();
        }else {down=true;if(tlsClient)tlsClient->onDisconnect();}});
        client.setMessageCallback([&](const auto&,auto* b){(void)tlsClient->onMessage(*b);});
        loop.runEvery(1ms,[&]{if(down && closed){client.stop();server.stop();loop.quit();}});
        loop.runAfter(5s,[]{GAMENET_TEST_FAIL("WS real TCP reconnect timed out");});client.connect();loop.loop();
        GAMENET_TEST_ASSERT(ready==1 && messages==1 && closed==1 && down && weak.expired());
    }
    auto serverFactory=[&](const auto& c,WebSocketCallbacks cb){
        auto out=std::make_shared<gamenet::test::WsStressServer>();WebSocketTlsOptions options;
        options.tls.bioCapacity=1024;options.tls.maxCipherSendChunkBytes=1024;
        options.tls.maxPendingPlaintextBytes=16384;options.tls.maxPendingWrites=4;
        auto s=WebSocketTlsSession::create(c,gamenet::tls::TlsContext::server(cert.certificate.string(),cert.key.string()),{
            .handshake=std::move(cb.handshake),.ready=std::move(cb.ready),.message=std::move(cb.message),.pong=std::move(cb.pong),
            .closed=[closed=std::move(cb.closed)](auto result){closed(result.graceful()?WebSocketError::Graceful:
                result.websocket==WebSocketError::Graceful?WebSocketError::Truncated:result.websocket);}
        },options);out->lifetime=s;
        out->start=[s]{(void)s->start();};out->down=[s]{s->onDisconnect();};out->writable=[s]{s->onWritable();};out->feed=[s](auto& b){(void)s->onMessage(b);};
        out->send=[s](auto type,auto b){return s->send(type,b);};out->close=[s]{return s->close();};out->abort=[s]{return s->abort();};
        out->snapshot=[s]{return s->snapshot().websocket;};out->bounded=[s]{const auto t=s->snapshot().tls;
            GAMENET_TEST_ASSERT(t.pendingPlaintextBytes<=16384 && t.pendingWrites<=4 && t.bufferedCipherInputBytes<=256*1024 && t.bufferedCipherOutputBytes<=1024 &&
                t.acceptedPlaintextBytes==t.encryptedPlaintextBytes+t.discardedPlaintextBytes+t.pendingPlaintextBytes);};out->settled=[s]{const auto t=s->snapshot().tls;
            GAMENET_TEST_ASSERT(!t.pendingPlaintextBytes && !t.pendingWrites && !t.bufferedCipherInputBytes && !t.bufferedCipherOutputBytes &&
                t.acceptedPlaintextBytes==t.encryptedPlaintextBytes+t.discardedPlaintextBytes);};return out;
    };
    auto clientFactory=[&](const auto& c,std::function<void()> ready,std::function<void(std::string_view)> data,std::function<void(bool)> closed){
        auto out=std::make_shared<gamenet::test::WsStressClient>();gamenet::tls::TlsOptions options;options.bioCapacity=1024;options.maxCipherSendChunkBytes=1024;
        auto s=gamenet::tls::TlsStream::create(c,gamenet::tls::TlsContext::client(cert.ca.string()),{
            .ready=std::move(ready),.data=std::move(data),.closed=[closed=std::move(closed)](auto error){closed(error==gamenet::tls::TlsError::Graceful);}
        },"localhost",options);out->lifetime=s;out->start=[s]{(void)s->start();};out->down=[s]{s->onDisconnect();};out->writable=[s]{s->onWritable();};
        out->shutdown=[s]{(void)s->shutdown();};out->feed=[s](auto& b){(void)s->onMessage(b);};out->send=[s](auto b){return s->trySend(b)==gamenet::tls::TlsOperation::Accepted;};
        out->bounded=[s]{const auto t=s->snapshot();GAMENET_TEST_ASSERT(t.pendingPlaintextBytes<=1024*1024 && t.pendingWrites<=256 && t.bufferedCipherInputBytes<=256*1024 && t.bufferedCipherOutputBytes<=1024 &&
            t.acceptedPlaintextBytes==t.encryptedPlaintextBytes+t.discardedPlaintextBytes+t.pendingPlaintextBytes);};
        out->settled=[s]{const auto t=s->snapshot();GAMENET_TEST_ASSERT(!t.pendingPlaintextBytes && !t.pendingWrites && !t.bufferedCipherInputBytes && !t.bufferedCipherOutputBytes &&
            t.acceptedPlaintextBytes==t.encryptedPlaintextBytes+t.discardedPlaintextBytes);};return out;
    };
    gamenet::test::runWebSocketReconnectStress(serverFactory,clientFactory,"WSS");
}
