// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/WebSocketWire.h"
#include "support/WebSocketStateSequence.h"
#include <thread>

using namespace gamenet::websocket;
using namespace gamenet::test;
using namespace std::chrono_literals;
namespace {
struct Fixture {
    gamenet::net::EventLoop loop;
    ConnectedSocketPair sockets{SocketPairMode::SmallSendBuffer};
    gamenet::net::TcpConnectionPtr connection;
    std::shared_ptr<WebSocketSession> session;
    unsigned ready{},messages{},closed{};
    WebSocketError error{};
    std::function<void(WebSocketOpcode,std::string_view)> handler;
    std::function<void()> onReady;
    std::function<bool(const WebSocketRequest&)> policy;
    bool releaseOnDown{};
    explicit Fixture(WebSocketOptions options={},std::size_t hardLimit=8192,gamenet::net::EventLoopOptions loops={})
        :loop(loops),connection(makeTcpConnection(loop,sockets,"ws-contract")) {
#ifdef _WIN32
        u_long mode=1;GAMENET_TEST_ASSERT(::ioctlsocket(sockets.peerFd,FIONBIO,&mode)==0);
#endif
        connection->setBackpressureOptions({.lowWaterMarkBytes=hardLimit/4,.highWaterMarkBytes=hardLimit/2,.hardLimitBytes=hardLimit});
        session=WebSocketSession::create(connection,{
            .handshake=[&](const auto& request){return policy?policy(request):true;},
            .ready=[&]{++ready;if(onReady)onReady();},
            .message=[&](auto type,auto bytes){++messages;if(handler)handler(type,bytes);},
            .closed=[&](auto reason){++closed;error=reason;}
        },options);
        connection->setConnectionCallback([&](const auto& c){if(!c->connected() && session){session->onDisconnect();if(releaseOnDown)session.reset();}});
        connection->setMessageCallback([&](const auto&,auto* input){if(session)(void)session->onMessage(*input);else input->retrieveAll();});
        connection->setCloseCallback([](const auto& c){c->connectDestroyed();});
        connection->setCallbackExceptionHandler([](const auto&,const auto&){GAMENET_TEST_FAIL("WebSocket callback escaped Core");});
        connection->connectEstablished();
    }
    void feed(std::string_view bytes){gamenet::net::Buffer input;input.append(bytes);(void)session->onMessage(input);GAMENET_TEST_ASSERT(!input.readableBytes());}
    void open(){GAMENET_TEST_ASSERT(session->start()==WebSocketOperation::Accepted);feed(websocketUpgrade);}
    void drain(){char bytes[16384];while(::recv(sockets.peerFd,bytes,sizeof(bytes),0)>0){}}
    void settled(){const auto s=session->snapshot();GAMENET_TEST_ASSERT(s.phase==WebSocketPhase::Closed && !s.inputBytes && !s.fragmentedBytes && !s.pendingControls && !s.pendingControlBytes && s.enqueuedControlBytes==s.admittedControlBytes+s.discardedControlBytes);}
    void run(std::function<bool()> done){
        loop.runEvery(1ms,[&,done]{if(done()){if(session)(void)session->abort();connection->forceClose();loop.quit();}});
        loop.runAfter(3s,[]{GAMENET_TEST_FAIL("WebSocket contract timed out");});loop.loop();
    }
    ~Fixture(){if(session)(void)session->abort();connection->forceClose();if(loop.phase()==gamenet::net::EventLoopPhase::Running){loop.quit();loop.loop();}}
};
void basicAndReentry(){
    Fixture f;f.onReady=[&]{
        gamenet::net::Buffer nested;nested.append(std::string_view("nested"));
        GAMENET_TEST_ASSERT(f.session->onMessage(nested)==WebSocketOperation::NotReady && nested.readableBytes()==6);
    };
    f.handler=[&](auto type,auto bytes){GAMENET_TEST_ASSERT(type==WebSocketOpcode::Text && bytes=="hello");
        GAMENET_TEST_ASSERT(f.session->send(type,bytes)==WebSocketOperation::Accepted);};
    GAMENET_TEST_ASSERT(f.session->send(WebSocketOpcode::Text,"early")==WebSocketOperation::NotReady);
    GAMENET_TEST_ASSERT(f.session->start()==WebSocketOperation::Accepted);
    // Upgrade and initial frames may share the same TCP Buffer.
    f.feed(websocketUpgrade+websocketClientFrame(1,"hel",false)+websocketClientFrame(9,"p")+websocketClientFrame(0,"lo"));
    GAMENET_TEST_ASSERT(f.ready==1 && f.messages==1 && f.session->snapshot().reentrantMessages==1);
    f.feed(websocketClientFrame(8,std::string("\x03\xe8",2)));
    bool peerClosed=false;
    f.run([&]{f.drain();if(!peerClosed && !f.connection->pendingOutputBytes()){f.sockets.closePeer();peerClosed=true;}return f.closed==1;});
    GAMENET_TEST_ASSERT(f.error==WebSocketError::Graceful && f.closed==1);f.settled();
}
void deadlinesAndMalformed(){
    {WebSocketOptions o;o.handshakeTimeout=10ms;Fixture f(o,32);f.open();
        GAMENET_TEST_ASSERT(!f.ready && f.session->snapshot().phase==WebSocketPhase::Upgrading && f.session->snapshot().pendingControls==1);
        f.run([&]{return f.closed==1;});GAMENET_TEST_ASSERT(f.error==WebSocketError::HandshakeTimeout && !f.ready);f.settled();}
    {WebSocketOptions o;o.handshakeTimeout=10ms;Fixture f(o);(void)f.session->start();f.run([&]{return f.closed==1;});
        GAMENET_TEST_ASSERT(!f.ready && f.error==WebSocketError::HandshakeTimeout);f.settled();}
    {WebSocketOptions o;o.closeTimeout=10ms;Fixture f(o);f.open();(void)f.session->close();f.run([&]{f.drain();return f.closed==1;});
        GAMENET_TEST_ASSERT(f.error==WebSocketError::CloseTimeout && f.session->snapshot().closeTimedOut);f.settled();}
    for(const auto& [wire,expected]:std::vector<std::pair<std::string,WebSocketError>>{
        {websocketClientFrame(1,"bad",true,false),WebSocketError::ProtocolError},
        {websocketClientFrame(1,"\xc0\x80"),WebSocketError::InvalidUtf8},
        {websocketClientFrame(2,std::string(17,'x')),WebSocketError::MessageTooLarge}}){
        WebSocketOptions o;o.codec.maxFrameBytes=16;Fixture f(o);f.open();f.feed(wire);f.connection->forceClose();
        f.run([&]{return f.closed==1;});GAMENET_TEST_ASSERT(f.error==expected);f.settled();}
    {Fixture f;(void)f.session->start();f.feed("GET / HTTP/1.1\r\nHost: x\r\n\r\n");f.connection->forceClose();
        f.run([&]{return f.closed==1;});GAMENET_TEST_ASSERT(!f.ready && f.error==WebSocketError::HandshakeRejected);f.settled();}
    {Fixture f;f.policy=[](const auto&){return false;};f.open();f.connection->forceClose();
        f.run([&]{return f.closed==1;});GAMENET_TEST_ASSERT(!f.ready && f.session->snapshot().httpStatus==403);f.settled();}
}
void backpressure(){
    {
    Fixture f;f.open();
    unsigned accepted=0,rejected=0;
    for(unsigned n=0;n<1024;++n){auto r=f.session->send(WebSocketOpcode::Binary,std::string(4096,'x'));
        if(r==WebSocketOperation::Overloaded){++rejected;break;}GAMENET_TEST_ASSERT(r==WebSocketOperation::Accepted);++accepted;}
    for(unsigned n=0;n<8192;++n)if(f.session->send(WebSocketOpcode::Binary,"")==WebSocketOperation::Overloaded)break;
    f.feed(websocketClientFrame(9,std::string(125,'p')));
    GAMENET_TEST_ASSERT(accepted && rejected && f.session->snapshot().pendingControls && f.session->snapshot().controlRetries);
    bool probe=false;
    f.run([&]{f.drain();const auto s=f.session->snapshot();
        GAMENET_TEST_ASSERT(s.pendingControlBytes<=1024 && f.connection->pendingOutputBytes()<=8192);
        if(!s.pendingControls && !f.connection->pendingOutputBytes() && !probe){probe=true;GAMENET_TEST_ASSERT(f.session->send(WebSocketOpcode::Text,"probe")==WebSocketOperation::Accepted);}
        return probe && !f.connection->pendingOutputBytes();});
    GAMENET_TEST_ASSERT(probe && f.closed==1);f.settled();
    }
    Fixture full;full.open();for(unsigned n=0;n<1024;++n)if(full.session->send(WebSocketOpcode::Binary,std::string(4096,'x'))==WebSocketOperation::Overloaded)break;
    for(unsigned n=0;n<8192;++n)if(full.session->send(WebSocketOpcode::Binary,"")==WebSocketOperation::Overloaded)break;
    for(unsigned n=0;n<5 && !full.closed;++n)full.feed(websocketClientFrame(9,std::string(125,'p')));
    GAMENET_TEST_ASSERT(full.closed==1 && full.error==WebSocketError::Backpressure);full.settled();
}
void eofAndScheduling(){
    {WebSocketOptions o;o.codec.maxFramesPerPush=1;Fixture f(o);f.open();
        std::weak_ptr<WebSocketSession> weak=f.session;f.releaseOnDown=true;
        f.feed(websocketClientFrame(1,"a")+websocketClientFrame(1,"b")+websocketClientFrame(1,"c"));
        f.connection->forceClose();
        f.run([&]{return f.messages==3 && f.closed==1 && weak.expired();});
        GAMENET_TEST_ASSERT(f.error==WebSocketError::Truncated && weak.expired());}
    {WebSocketOptions o;o.codec.maxFramesPerPush=1;Fixture f(o,8192,{.maxPendingFunctors=1,.reservedPendingFunctors=0,.maxFunctorsPerIteration=1});f.open();
        GAMENET_TEST_ASSERT(f.loop.executor().post([]{})==gamenet::net::PostResult::Accepted);
        f.feed(websocketClientFrame(1,"a")+websocketClientFrame(1,"b"));
        GAMENET_TEST_ASSERT(f.closed==1 && f.error==WebSocketError::SchedulingRejected && f.messages==1);f.settled();}
}
void foreignAndRelease(){
    {Fixture f;std::thread other([&]{
        GAMENET_TEST_ASSERT(f.session->start()==WebSocketOperation::WrongThread && f.session->send(WebSocketOpcode::Text,"x")==WebSocketOperation::WrongThread &&
            f.session->close()==WebSocketOperation::WrongThread && f.session->abort()==WebSocketOperation::WrongThread);
    });other.join();f.open();f.handler=[&](auto,auto){(void)f.session->abort();f.session.reset();};
        auto lease=f.session;f.feed(websocketClientFrame(1,"release"));GAMENET_TEST_ASSERT(f.closed==1 && f.error==WebSocketError::Aborted);lease.reset();}
    {Fixture f;f.open();f.handler=[](auto,auto){throw std::runtime_error("contained");};f.feed(websocketClientFrame(1,"throw"));f.connection->forceClose();
        f.run([&]{return f.closed==1;});GAMENET_TEST_ASSERT(f.error==WebSocketError::CallbackException);f.settled();}
    {Fixture f;f.open();f.session.reset();GAMENET_TEST_ASSERT(f.closed==1 && f.error==WebSocketError::Aborted);}
}
void transportSeam(){
    struct Transport final : WebSocketTransport {
        gamenet::net::EventLoop* loop;
        WebSocketTransportState phase{WebSocketTransportState::NotReady};
        std::function<bool(std::string_view)> sender;
        unsigned abandoned{};
        explicit Transport(gamenet::net::EventLoop* owner):loop(owner){}
        gamenet::net::EventLoop* ownerLoop() const noexcept override{return loop;}
        WebSocketTransportState state() const noexcept override{return phase;}
        bool trySend(std::string_view wire) override{return sender?sender(wire):true;}
        bool shutdown() override{return true;}
        void abort() override{}
        void abandon() override{++abandoned;}
        bool cleanClose() const noexcept override{return false;}
    };
    gamenet::net::EventLoop loop;auto transport=std::make_shared<Transport>(&loop);unsigned ready=0,closed=0;
    auto session=WebSocketSession::createWithTransport(transport,{.ready=[&]{++ready;},.closed=[&](auto){++closed;}});
    GAMENET_TEST_ASSERT(session->start()==WebSocketOperation::NotReady && session->snapshot().phase==WebSocketPhase::Created);
    gamenet::net::Buffer input;input.append(websocketUpgrade);
    GAMENET_TEST_ASSERT(session->onMessage(input)==WebSocketOperation::NotReady && input.readableBytes()==websocketUpgrade.size());
    transport->phase=WebSocketTransportState::Ready;
    GAMENET_TEST_ASSERT(session->start()==WebSocketOperation::Accepted);
    transport->sender=[&](auto){transport->phase=WebSocketTransportState::Closed;session->onDisconnect();return true;};
    (void)session->onMessage(input); // terminal callback clears controls before trySend returns
    GAMENET_TEST_ASSERT(!ready && closed==1 && session->snapshot().phase==WebSocketPhase::Closed && !session->snapshot().pendingControlBytes);
    auto other=WebSocketSession::createWithTransport(transport,{.closed=[&](auto){++closed;}});
    GAMENET_TEST_ASSERT(other->abandon()==WebSocketOperation::Accepted && transport->abandoned==1 && closed==2);
}
} // namespace
int main(){basicAndReentry();deadlinesAndMalformed();backpressure();eofAndScheduling();foreignAndRelease();transportSeam();verifyWebSocketStateSeeds();}
