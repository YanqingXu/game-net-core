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
#include <optional>

using namespace gamenet::websocket;
namespace {
struct Diagnostic {
    std::weak_ptr<WebSocketTlsSession> session;
    std::weak_ptr<gamenet::net::TcpConnection> connection;
    std::optional<WebSocketTlsSnapshot> last;
    unsigned peer{}, sequence{}, reads{}, terminals{};
    void record(const char* event) {
        if(sequence>=128 && (std::string_view(event)=="cipher-input" || std::string_view(event)=="tcp-writable"))return;
        if(auto s=session.lock())last=s->snapshot();
        const auto c=connection.lock();
        const auto info=c?c->closeInfo():std::nullopt;
        std::cout<<"TRACE {\"event\":\""<<event<<"\",\"peer_port\":"<<peer
            <<",\"sequence\":"<<++sequence<<",\"reads\":"<<reads<<",\"terminals\":"<<terminals
            <<",\"tcp_pending\":"<<(c?c->pendingOutputBytes():0)
            <<",\"tcp_phase\":"<<(c?int(c->closePhase()):-1)
            <<",\"tcp_reason\":"<<(info?int(info->reason):-1)
            <<",\"tcp_error\":"<<(info?info->nativeError:0);
        if(last){const auto& s=*last;
            std::cout<<",\"wss_phase\":"<<int(s.phase)<<",\"ws_phase\":"<<int(s.websocket.phase)
                <<",\"ws_terminal\":"<<int(s.websocket.terminal)
                <<",\"ws_sent_close\":"<<s.websocket.sentClose<<",\"ws_received_close\":"<<s.websocket.receivedClose
                <<",\"ws_controls\":"<<s.websocket.pendingControlBytes<<",\"ws_input\":"<<s.websocket.inputBytes
                <<",\"ws_fragmented\":"<<s.websocket.fragmentedBytes<<",\"ws_pending_controls\":"<<s.websocket.pendingControls
                <<",\"ws_continuation\":"<<s.websocket.continuationQueued
                <<",\"ws_enqueued\":"<<s.websocket.enqueuedControlBytes<<",\"ws_admitted\":"<<s.websocket.admittedControlBytes
                <<",\"ws_discarded\":"<<s.websocket.discardedControlBytes
                <<",\"tls_phase\":"<<int(s.tls.phase)<<",\"tls_terminal\":"<<int(s.tls.terminal)
                <<",\"tls_sent_notify\":"<<s.tls.sentCloseNotify<<",\"tls_received_notify\":"<<s.tls.receivedCloseNotify
                <<",\"tls_accepted\":"<<s.tls.acceptedPlaintextBytes<<",\"tls_encrypted\":"<<s.tls.encryptedPlaintextBytes
                <<",\"tls_discarded\":"<<s.tls.discardedPlaintextBytes<<",\"tls_pending\":"<<s.tls.pendingPlaintextBytes
                <<",\"tls_pending_writes\":"<<s.tls.pendingWrites
                <<",\"tls_input\":"<<s.tls.bufferedCipherInputBytes<<",\"tls_output\":"<<s.tls.bufferedCipherOutputBytes;
        }
        std::cout<<'}'<<std::endl;
    }
};
struct Peer {
    std::shared_ptr<WebSocketTlsSession> session;
    std::shared_ptr<Diagnostic> diagnostic;
};
}
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
                const auto* value=std::any_cast<Peer>(&c->getContext());
                if(value){const auto peer=*value;peer.diagnostic->record("tcp-down-before");peer.session->onDisconnect();peer.diagnostic->record("tcp-down-after");}
                c->setContext(std::any{});return;
            }
            auto diagnostic=std::make_shared<Diagnostic>();diagnostic->connection=c;diagnostic->peer=c->peerAddress().port();
            WebSocketTlsOptions options;options.websocket.codec.maxFrameBytes=256*1024;
            auto session=WebSocketTlsSession::create(c,context,{
                .handshake=[](const auto& request){return request.target=="/echo";},
                .ready=[diagnostic]{diagnostic->record("ws-ready");},
                .message=[weak=std::weak_ptr(c)](auto type,auto payload){
                    if(auto connection=weak.lock()){
                        const auto peer=std::any_cast<Peer>(connection->getContext());
                        if(peer.session->send(type,payload)!=WebSocketOperation::Accepted)(void)peer.session->close(1013,"backpressure");
                    }
                },
                .closed=[diagnostic](auto result){++diagnostic->terminals;diagnostic->record("closed");
                    std::cout<<"CLOSED "<<int(result.websocket)<<' '<<int(result.tls)<<' '<<result.graceful()<<std::endl;}
            },options);diagnostic->session=session;c->setContext(Peer{session,diagnostic});
            diagnostic->record("tcp-up");(void)session->start();
        });
        server.setMessageCallback([](const auto& c,auto* input){
            const auto* value=std::any_cast<Peer>(&c->getContext());
            if(value){const auto peer=*value;++peer.diagnostic->reads;(void)peer.session->onMessage(*input);
                if(peer.diagnostic->reads<=64)peer.diagnostic->record("cipher-input");}
            else input->retrieveAll();
        });
        server.setWriteCompleteCallback([](const auto& c){
            const auto* value=std::any_cast<Peer>(&c->getContext());if(value){const auto peer=*value;peer.session->onWritable();
                if(peer.diagnostic->reads<=64)peer.diagnostic->record("tcp-writable");}
        });
        server.start();std::cout<<"PORT "<<server.listenAddress().port()<<std::endl;
        if(seconds)loop.runAfter(std::chrono::seconds(seconds),[&]{server.stop();loop.quit();});
        loop.loop();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
