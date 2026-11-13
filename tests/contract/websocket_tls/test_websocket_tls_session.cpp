// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "support/WebSocketTlsFixture.h"
#include "support/TestAssert.h"
#include <thread>
using namespace gamenet::websocket;
using namespace gamenet::tls;
using namespace gamenet::test;
using namespace std::chrono_literals;
namespace {
void handshakeAndClose(TlsCertificates& cert) {
    WebSocketTlsFixture f(cert);
    GAMENET_TEST_ASSERT(f.server->send(WebSocketOpcode::Text,"early")==WebSocketOperation::NotReady);
    f.upgrade+=websocketClientFrame(1,"hel",false)+websocketClientFrame(9,"p")+websocketClientFrame(0,"lo");
    f.message=[&](auto type,auto bytes){GAMENET_TEST_ASSERT(type==WebSocketOpcode::Text && bytes=="hello" && f.serverReady==1);
        GAMENET_TEST_ASSERT(f.server->send(type,bytes)==WebSocketOperation::Accepted);};
    bool sentClose=false,tlsClose=false;
    f.clientData=[&](auto){
        if(!sentClose && f.wire.find("\x81\x05hello")!=std::string::npos){
            GAMENET_TEST_ASSERT(f.wire.starts_with("HTTP/1.1 101") && f.wire.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")!=std::string::npos);
            sentClose=true;GAMENET_TEST_ASSERT(f.client->trySend(websocketClientFrame(8,std::string("\x03\xe8",2)))==TlsOperation::Accepted);
        }
        if(sentClose && !tlsClose && f.wire.find(std::string("\x88\x02\x03\xe8",4))!=std::string::npos){tlsClose=true;(void)f.client->shutdown();}
    };
    f.run([&]{return !f.serverClosed.empty() && !f.clientClosed.empty();});
    GAMENET_TEST_ASSERT(f.serverReady==1 && f.policies==1 && f.messages==1 && f.serverClosed[0].graceful() &&
        f.clientClosed==std::vector{TlsError::Graceful});f.settled();
}
void certificateAndTimeouts(TlsCertificates& cert) {
    for(unsigned mode=0;mode<3;++mode){
        WebSocketTlsFixture f(cert,{}, {},mode==1?"wrong.example":"localhost",mode==0,mode==2);
        f.run([&]{return !f.serverClosed.empty() && !f.clientClosed.empty();});
        GAMENET_TEST_ASSERT(!f.serverReady && !f.policies && !f.messages && f.clientClosed==std::vector{TlsError::CertificateRejected} && !f.serverClosed[0].graceful());f.settled();
    }
    {WebSocketTlsOptions o;o.tls.handshakeTimeout=10ms;WebSocketTlsFixture f(cert,o);
        f.run([&]{return !f.serverClosed.empty();},false);
        GAMENET_TEST_ASSERT(!f.serverReady && !f.policies && f.serverClosed[0].tls==TlsError::HandshakeTimeout);f.settled();}
    {WebSocketTlsOptions o;o.websocket.handshakeTimeout=10ms;WebSocketTlsFixture f(cert,o);f.autoUpgrade=false;
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.clientReady==1 && !f.serverReady && !f.policies && f.serverClosed[0].websocket==WebSocketError::HandshakeTimeout);f.settled();}
    {WebSocketTlsOptions o;o.websocket.handshakeTimeout=15ms;o.tls.maxPendingPlaintextBytes=32;WebSocketTlsFixture f(cert,o);
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(!f.serverReady && f.policies==1 && f.serverClosed[0].websocket==WebSocketError::HandshakeTimeout &&
            f.server->snapshot().websocket.controlRetries>0);f.settled();}
    {WebSocketTlsOptions o;o.websocket.closeTimeout=10ms;WebSocketTlsFixture f(cert,o);
        f.ready=[&]{GAMENET_TEST_ASSERT(f.server->close()==WebSocketOperation::Accepted);};
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed[0].websocket==WebSocketError::CloseTimeout);f.settled();}
    {WebSocketTlsOptions o;o.tls.shutdownTimeout=15ms;WebSocketTlsFixture f(cert,o);bool sent=false;
        f.clientData=[&](auto){if(!sent && f.wire.find("\r\n\r\n")!=std::string::npos){sent=true;
            GAMENET_TEST_ASSERT(f.client->trySend(websocketClientFrame(8,std::string("\x03\xe8",2)))==TlsOperation::Accepted);
            (void)f.client->pauseRead();}};
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed[0].tls==TlsError::ShutdownTimeout && !f.serverClosed[0].graceful());f.settled();}
}
void serverCloseAndConstruction(TlsCertificates& cert) {
    {WebSocketTlsFixture f(cert);bool replied=false;
        f.ready=[&]{GAMENET_TEST_ASSERT(f.server->close()==WebSocketOperation::Accepted);};
        f.clientData=[&](auto){if(!replied && f.wire.find(std::string("\x88\x02\x03\xe8",4))!=std::string::npos){replied=true;
            GAMENET_TEST_ASSERT(f.client->trySend(websocketClientFrame(8,std::string("\x03\xe8",2)))==TlsOperation::Accepted);
            (void)f.client->shutdown();}};
        f.closed=[](auto result){GAMENET_TEST_ASSERT(result.graceful());throw std::runtime_error("contained combined close");};
        f.run([&]{return !f.serverClosed.empty() && !f.clientClosed.empty();});
        GAMENET_TEST_ASSERT(replied && f.serverClosed[0].graceful() && f.server->snapshot().callbackExceptions==1);f.settled();
    }
    WebSocketTlsFixture f(cert);unsigned callbacks=0;
    for(unsigned mode=0;mode<3;++mode){WebSocketTlsOptions options;
        if(mode==0)options.websocket.maxControlFrames=0;
        if(mode==1)options.tls.maxPendingWrites=0;
        try {
            (void)WebSocketTlsSession::create(f.serverConnection,
                mode==2?TlsContext::client(cert.ca.string()):TlsContext::server(cert.certificate.string(),cert.key.string()),
                {.closed=[&](auto){++callbacks;}},options);
            GAMENET_TEST_FAIL("invalid WSS construction accepted");
        }catch(const std::invalid_argument&){}
        GAMENET_TEST_ASSERT(callbacks==0 && f.serverConnection->connected());
    }
}
void backpressure(TlsCertificates& cert) {
    WebSocketTlsOptions o;o.tls.maxPendingWrites=1;o.websocket.codec.maxFrameBytes=256*1024;
    WebSocketTlsFixture f(cert,o,{},"localhost",false,false,8192);f.forwardWritable=false;
    const std::string payload(128*1024,'b');bool rejected=false,probe=false;
    const auto encoded=WebSocketCodec::encodeServer(WebSocketOpcode::Binary,payload,256*1024);
    GAMENET_TEST_ASSERT(encoded);
    f.message=[&](auto,auto){GAMENET_TEST_ASSERT(f.server->send(WebSocketOpcode::Binary,payload)==WebSocketOperation::Accepted);
        rejected=f.server->send(WebSocketOpcode::Text,"refused")==WebSocketOperation::Overloaded;};
    bool pingSent=false;
    f.clientData=[&](auto){
        if(!pingSent && f.wire.find("\r\n\r\n")!=std::string::npos){pingSent=true;(void)f.client->trySend(websocketClientFrame(1,"trigger")+websocketClientFrame(9,"recover"));}
        if(!probe && f.wire.find(*encoded)!=std::string::npos &&
            f.wire.find(*WebSocketCodec::encodeServer(WebSocketOpcode::Pong,"recover"))!=std::string::npos){probe=true;
            GAMENET_TEST_ASSERT(f.server->send(WebSocketOpcode::Text,"probe")==WebSocketOperation::Accepted);}
    };
    f.run([&]{return probe && f.wire.find(*WebSocketCodec::encodeServer(WebSocketOpcode::Text,"probe"))!=std::string::npos;});
    GAMENET_TEST_ASSERT(rejected && f.server->snapshot().tls.transportRejections>0 && f.server->snapshot().websocket.controlRetries>0);f.settled();
}
void terminalPaths(TlsCertificates& cert) {
    {WebSocketTlsFixture f(cert);f.policy=[](const auto&){return false;};
        f.run([&]{return !f.serverClosed.empty();});GAMENET_TEST_ASSERT(!f.serverReady && f.serverClosed[0].websocket==WebSocketError::HandshakeRejected && f.wire.starts_with("HTTP/1.1 403"));f.settled();}
    {WebSocketTlsFixture f(cert);f.ready=[&]{f.clientConnection->forceClose();};
        f.run([&]{return !f.serverClosed.empty();});GAMENET_TEST_ASSERT(!f.serverClosed[0].graceful());f.settled();}
    {WebSocketTlsFixture f(cert);f.ready=[&]{(void)f.client->shutdown();};
        f.run([&]{return !f.serverClosed.empty();});GAMENET_TEST_ASSERT(f.serverClosed[0].websocket==WebSocketError::Truncated && !f.serverClosed[0].graceful());f.settled();}
    {WebSocketTlsFixture f(cert);f.message=[](auto,auto){throw std::runtime_error("WS callback");};
        f.upgrade+=websocketClientFrame(1,"throw");f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed[0].websocket==WebSocketError::CallbackException);f.settled();}
    {WebSocketTlsFixture f(cert);std::thread other([&]{
        GAMENET_TEST_ASSERT(f.server->start()==TlsOperation::WrongThread && f.server->send(WebSocketOpcode::Text,"x")==WebSocketOperation::WrongThread &&
            f.server->close()==WebSocketOperation::WrongThread && f.server->abort()==WebSocketOperation::WrongThread);
    });other.join();f.ready=[&]{(void)f.server->abort();f.server.reset();};f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed.size()==1 && f.serverClosed[0].websocket==WebSocketError::Aborted);}
    {WebSocketTlsFixture f(cert);f.ready=[&]{f.server.reset();};f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed.size()==1 && f.serverClosed[0].websocket==WebSocketError::Aborted && f.serverClosed[0].tls==TlsError::Aborted);}
}
void eofLeaseAndScheduling(TlsCertificates& cert) {
    {WebSocketTlsFixture f(cert,{}, {},"localhost",false,false,65536,
            {.maxPendingFunctors=1,.reservedPendingFunctors=0,.maxFunctorsPerIteration=1});
        f.upgrade+=websocketClientFrame(1,"occupy queue")+websocketClientFrame(8,std::string("\x03\xe8",2));
        f.message=[&](auto,auto){GAMENET_TEST_ASSERT(f.loop.executor().post([]{})==gamenet::net::PostResult::Accepted);};
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.serverClosed[0].websocket==WebSocketError::SchedulingRejected &&
            f.server->snapshot().websocket.sentClose && !f.serverClosed[0].graceful());f.settled();
    }
    {WebSocketTlsOptions o;o.websocket.codec.maxFramesPerPush=1;WebSocketTlsFixture f(cert,o);
        std::weak_ptr<WebSocketTlsSession> weak=f.server;f.releaseOnDown=true;
        for(unsigned n=0;n<8;++n)f.upgrade+=websocketClientFrame(1,"accepted");
        f.message=[&](auto,auto){if(f.messages==1){const auto lease=f.server;f.serverConnection->forceClose();
            GAMENET_TEST_ASSERT(lease->send(WebSocketOpcode::Text,"late")==WebSocketOperation::Closed);}};
        f.run([&]{return f.messages==8 && f.serverClosed.size()==1 && weak.expired();});
        GAMENET_TEST_ASSERT(f.messages==8 && weak.expired() && !f.serverClosed[0].graceful());
    }
    {WebSocketTlsOptions o;o.websocket.codec.maxFramesPerPush=1;
        WebSocketTlsFixture f(cert,o,{},"localhost",false,false,65536,
            {.maxPendingFunctors=1,.reservedPendingFunctors=0,.maxFunctorsPerIteration=1});
        f.upgrade+=websocketClientFrame(1,"a")+websocketClientFrame(1,"b");
        f.message=[&](auto,auto){
            const auto result=f.loop.executor().post([]{});
            GAMENET_TEST_ASSERT(result==gamenet::net::PostResult::Accepted || result==gamenet::net::PostResult::QueueFull);
        };
        f.run([&]{return !f.serverClosed.empty();});
        GAMENET_TEST_ASSERT(f.messages==1 && f.serverClosed[0].websocket==WebSocketError::SchedulingRejected);f.settled();
    }
}
void closeInterleavings(TlsCertificates& cert) {
    // Independent masked wire fragments cross separate TLS records. The peer
    // may pipeline its close_notify immediately or wait for the echoed Close.
    for(unsigned mode=0;mode<6;++mode){
        WebSocketTlsOptions options;options.tls.bioCapacity=128;
        options.tls.maxStepsPerDrive=2;options.tls.maxCipherSendChunkBytes=31;
        options.websocket.codec.maxFramesPerPush=1;
        TlsOptions clientOptions=options.tls;
        WebSocketTlsFixture f(cert,options,clientOptions);
        const auto frame=websocketClientFrame(8,std::string("\x03\xe8",2));
        const auto expected=std::string("\x88\x02\x03\xe8",4);
        bool closeSent=false,notifySent=false;
        if(mode==1)f.ready=[&]{GAMENET_TEST_ASSERT(f.server->close()==WebSocketOperation::Accepted);};
        f.clientData=[&](auto){
            if(!closeSent && f.wire.find("\r\n\r\n")!=std::string::npos){
                closeSent=true;
                if(mode==3){notifySent=true;(void)f.client->shutdown();return;}
                const auto prefix=mode==4?frame.size()-1:std::size_t{1};
                GAMENET_TEST_ASSERT(f.client->trySend(std::string_view(frame).substr(0,prefix))==TlsOperation::Accepted);
                if(mode!=4)GAMENET_TEST_ASSERT(f.client->trySend(std::string_view(frame).substr(prefix))==TlsOperation::Accepted);
                if(mode==5){f.clientConnection->shutdown();return;}
                if(mode!=2){notifySent=true;(void)f.client->shutdown();}
            }
            if(mode==2 && !notifySent && f.wire.find(expected)!=std::string::npos){notifySent=true;(void)f.client->shutdown();}
        };
        f.run([&]{return !f.serverClosed.empty() && !f.clientClosed.empty();});
        GAMENET_TEST_ASSERT(closeSent && f.serverClosed.size()==1 && f.clientClosed.size()==1);
        const auto snapshot=f.server->snapshot();
        if(mode<3){
            GAMENET_TEST_ASSERT(notifySent && f.wire.find(expected)!=std::string::npos &&
                f.serverClosed[0].graceful() && f.clientClosed[0]==TlsError::Graceful &&
                snapshot.websocket.sentClose && snapshot.websocket.receivedClose &&
                snapshot.tls.sentCloseNotify && snapshot.tls.receivedCloseNotify);
        }else{
            GAMENET_TEST_ASSERT(!f.serverClosed[0].graceful());
            if(mode==3 || mode==4)GAMENET_TEST_ASSERT(!snapshot.websocket.receivedClose);
            if(mode==5)GAMENET_TEST_ASSERT(snapshot.tls.terminal==TlsError::Truncated);
        }
        f.settled();
    }
}
}
int main(){TlsCertificates cert;handshakeAndClose(cert);certificateAndTimeouts(cert);serverCloseAndConstruction(cert);backpressure(cert);terminalPaths(cert);eofLeaseAndScheduling(cert);closeInterleavings(cert);}
