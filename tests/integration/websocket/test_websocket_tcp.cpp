// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/TestAssert.h"
#include "support/WebSocketWire.h"
#include <any>
#include <vector>
using namespace gamenet::websocket;
using namespace std::chrono_literals;
int main(){
    for(unsigned generation=0;generation<8;++generation){
        gamenet::net::EventLoop loop;unsigned ready=0,messages=0,closed=0;bool down=false;
        std::weak_ptr<WebSocketSession> weak;
        gamenet::net::TcpServer server(&loop,gamenet::net::InetAddress(0,true),"ws-integration");
        server.setConnectionCallback([&](const auto& c){
            if(!c->connected()){auto s=std::any_cast<std::shared_ptr<WebSocketSession>>(c->getContext());s->onDisconnect();c->setContext(std::any{});return;}
            auto s=WebSocketSession::create(c,{
                .ready=[&]{++ready;},
                .message=[&,weakConnection=std::weak_ptr(c)](auto type,auto payload){++messages;auto connection=weakConnection.lock();
                    auto session=std::any_cast<std::shared_ptr<WebSocketSession>>(connection->getContext());
                    GAMENET_TEST_ASSERT(session->send(type,payload)==WebSocketOperation::Accepted);},
                .closed=[&](auto error){GAMENET_TEST_ASSERT(error==WebSocketError::Graceful);++closed;}
            });weak=s;c->setContext(s);(void)s->start();
        });
        server.setMessageCallback([](const auto& c,auto* b){auto s=std::any_cast<std::shared_ptr<WebSocketSession>>(c->getContext());(void)s->onMessage(*b);});
        server.start();gamenet::net::TcpClient client(&loop,server.listenAddress(),"independent-wire-client");
        std::string received;bool sent=false,closeSent=false;
        const auto payload=std::string("binary\0",7)+std::string(4096,'x');
        client.setConnectionCallback([&](const auto& c){if(c->connected()){
            c->send(std::string_view(gamenet::test::websocketUpgrade).substr(0,7));
            loop.runAfter(1ms,[c]{c->send(std::string_view(gamenet::test::websocketUpgrade).substr(7));});
        }else down=true;});
        client.setMessageCallback([&](const auto& c,auto* b){received.append(b->peek(),b->readableBytes());b->retrieveAll();
            if(!sent && received.find("\r\n\r\n")!=std::string::npos){
                GAMENET_TEST_ASSERT(received.starts_with("HTTP/1.1 101") && received.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")!=std::string::npos);
                received.erase(0,received.find("\r\n\r\n")+4);sent=true;
                c->send(gamenet::test::websocketClientFrame(2,std::string_view(payload).substr(0,3),false)+
                    gamenet::test::websocketClientFrame(9,"ping")+gamenet::test::websocketClientFrame(0,std::string_view(payload).substr(3)));
            }
            const auto expected=*WebSocketCodec::encodeServer(WebSocketOpcode::Pong,"ping")+*WebSocketCodec::encodeServer(WebSocketOpcode::Binary,payload);
            if(sent && !closeSent && received.size()>=expected.size()){
                GAMENET_TEST_ASSERT(received==expected);received.clear();closeSent=true;
                c->send(gamenet::test::websocketClientFrame(8,std::string("\x03\xe8",2)));
            }
            if(closeSent && received.size()>=4){GAMENET_TEST_ASSERT(received==std::string("\x88\x02\x03\xe8",4));c->shutdown();}
        });
        loop.runEvery(1ms,[&]{if(down && closed){client.stop();server.stop();loop.quit();}});
        loop.runAfter(5s,[]{GAMENET_TEST_FAIL("WS real TCP reconnect timed out");});client.connect();loop.loop();
        GAMENET_TEST_ASSERT(ready==1 && messages==1 && closed==1 && down && weak.expired());
    }
}
