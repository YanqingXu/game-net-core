// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Non-installed independent-client integration peer.
#include "gamenet/websocket_tls/WebSocketTlsSession.h"
#include "support/TestAssert.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include <any>
#include <iostream>

using namespace gamenet::websocket;
int main(int argc,char** argv){
    try {
        if(argc<3) {std::cerr<<"usage: wss-echo certificate.pem key.pem [port] [seconds]\n";return 2;}
        const auto context=gamenet::tls::TlsContext::server(argv[1],argv[2]);
        const int port=argc>3?std::stoi(argv[3]):7002;
        const int seconds=argc>4?std::stoi(argv[4]):0;
        if(argc>5 || port<0 || port>65535 || seconds<0 || seconds>3600) return 2;
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(&loop,gamenet::net::InetAddress(static_cast<std::uint16_t>(port),true),"websocket-echo");
        server.setConnectionBackpressureOptions({.lowWaterMarkBytes=64*1024,.highWaterMarkBytes=128*1024,.hardLimitBytes=256*1024});
        server.setConnectionCallback([context](const auto& c){
            if(!c->connected()){
                const auto* value=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(&c->getContext());
                if(value)(*value)->onDisconnect();c->setContext(std::any{});return;
            }
            WebSocketTlsOptions options;options.websocket.codec.maxFrameBytes=256*1024;
            auto session=WebSocketTlsSession::create(c,context,{
                .handshake=[](const auto& request){return request.target=="/echo";},
                .message=[weak=std::weak_ptr(c)](auto type,auto payload){
                    if(auto connection=weak.lock()){
                        const auto session=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(connection->getContext());
                        if(session->send(type,payload)!=WebSocketOperation::Accepted)(void)session->close(1013,"backpressure");
                    }
                },
                .closed=[](auto result){std::cout<<"CLOSED "<<int(result.websocket)<<' '<<int(result.tls)<<' '<<result.graceful()<<std::endl;}
            },options);c->setContext(session);(void)session->start();
        });
        server.setMessageCallback([](const auto& c,auto* input){
            const auto* session=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(&c->getContext());
            if(session)(void)(*session)->onMessage(*input);else input->retrieveAll();
        });
        server.setWriteCompleteCallback([](const auto& c){
            const auto* session=std::any_cast<std::shared_ptr<WebSocketTlsSession>>(&c->getContext());if(session)(*session)->onWritable();
        });
        server.start();std::cout<<"PORT "<<server.listenAddress().port()<<std::endl;
        if(seconds)loop.runAfter(std::chrono::seconds(seconds),[&]{server.stop();loop.quit();});
        loop.loop();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
