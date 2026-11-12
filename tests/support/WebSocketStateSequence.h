// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "support/TestAssert.h"
#include "support/WebSocketWire.h"
#include <array>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace gamenet::test {
// Admission-only transport; actual session/timer/continuation state uses a real
// owner. Receipts and callback ledgers do not use production codec decisions.
class WebSocketStateSequence {
    using Session=websocket::WebSocketSession;
    using Phase=websocket::WebSocketPhase;
    using Operation=websocket::WebSocketOperation;
    using Opcode=websocket::WebSocketOpcode;
    struct Transport final : websocket::WebSocketTransport {
        net::EventLoop* loop;
        websocket::WebSocketTransportState phase{websocket::WebSocketTransportState::Ready};
        std::weak_ptr<Session> session;
        std::vector<std::string> pending;
        std::uint64_t accepted{},drained{},discarded{},appFrames{},controlBytes{};
        std::size_t bytes{};
        unsigned mode{},shutdowns{};
        bool clean{};
        explicit Transport(net::EventLoop& owner):loop(&owner){}
        net::EventLoop* ownerLoop() const noexcept override{return loop;}
        websocket::WebSocketTransportState state() const noexcept override{return phase;}
        static bool control(std::string_view wire) {
            if(wire.starts_with("HTTP/1.1 ")) {
                GAMENET_TEST_ASSERT(wire.size()<512 && wire.ends_with("\r\n\r\n"));return true;
            }
            GAMENET_TEST_ASSERT(wire.size()>=2);
            const auto a=static_cast<unsigned char>(wire[0]),b=static_cast<unsigned char>(wire[1]);
            GAMENET_TEST_ASSERT((a&0xf0)==0x80 && !(b&0x80));
            const auto op=a&15;GAMENET_TEST_ASSERT(op==1 || op==2 || op==8 || op==9 || op==10);
            std::uint64_t length=b&127;std::size_t header=2;
            if(length==126 || length==127) {
                const auto n=length==126?2u:8u;GAMENET_TEST_ASSERT(wire.size()>=2+n);length=0;
                for(unsigned i=0;i<n;++i)length=(length<<8)|static_cast<unsigned char>(wire[2+i]);
                GAMENET_TEST_ASSERT(length>=(n==2?126u:65536u));header+=n;
            }
            GAMENET_TEST_ASSERT(length==wire.size()-header && (op<8 || length<=125));
            return op==8 || op==10;
        }
        bool trySend(std::string_view wire) override {
            const bool accept=phase==websocket::WebSocketTransportState::Ready && mode!=1 && mode!=3 && wire.size()<=256-bytes;
            if(accept) {
                if(control(wire))controlBytes+=wire.size();else ++appFrames;
                pending.emplace_back(wire);bytes+=wire.size();accepted+=wire.size();
            }
            if(mode==2 || mode==3) down(false); // synchronous terminal reentry
            return accept;
        }
        void drain(){drained+=bytes;bytes=0;pending.clear();}
        void discard(){discarded+=bytes;bytes=0;pending.clear();}
        void down(bool orderly) {
            if(orderly)drain();else discard();clean=orderly;
            phase=websocket::WebSocketTransportState::Closed;
            if(auto s=session.lock())s->onDisconnect();
        }
        bool shutdown() override {++shutdowns;phase=websocket::WebSocketTransportState::Closed;return true;}
        void abort() override {phase=websocket::WebSocketTransportState::Closed;discard();}
        void abandon() override {abort();}
        bool cleanClose() const noexcept override{return clean;}
    };
    struct Record {
        std::weak_ptr<Session> session;
        std::shared_ptr<Transport> transport;
        unsigned ready{},closed{},messages{},pongs{},offered{};
    };
    net::EventLoop loop_{{.maxPendingFunctors=8,.reservedPendingFunctors=0,.maxFunctorsPerIteration=4}};
    websocket::WebSocketOptions options_;
    std::array<Record,256> records_{};
    std::shared_ptr<Session> session_;
    std::vector<std::uint8_t> input_;
    std::size_t generation_{},offset_{};
    unsigned callbackMode_{};
    bool rejectPolicy_{},nested_{},done_{};

    void callback(std::size_t id) {
        if(nested_)return;
        nested_=true;const auto s=records_[id].session.lock();
        if(s)switch(callbackMode_) {
        case 1:(void)s->send(Opcode::Text,"callback");break;
        case 2:(void)s->close();break;
        case 3:(void)s->abort();break;
        case 4:if(id==generation_)session_.reset();break;
        case 5:{net::Buffer bytes;bytes.append(std::string_view("nested"));
            GAMENET_TEST_ASSERT(s->onMessage(bytes)==Operation::NotReady && bytes.readableBytes()==6);s->onWritable();break;}
        case 6:nested_=false;throw std::runtime_error("injected callback failure");
        case 7:records_[id].transport->down(false);break;
        }
        nested_=false;
    }
    void check(std::size_t id) {
        const auto& r=records_[id];const auto& t=*r.transport;
        GAMENET_TEST_ASSERT(r.ready<=1 && r.closed<=1 && t.accepted==t.drained+t.discarded+t.bytes && t.bytes<=256 && t.shutdowns<=1);
        if(const auto s=r.session.lock()) {
            const auto v=s->snapshot();
            GAMENET_TEST_ASSERT(v.receivedMessages==r.messages && v.acceptedSends==t.appFrames && r.messages<=r.offered);
            GAMENET_TEST_ASSERT(v.pendingControlBytes<=options_.maxControlBytes && v.pendingControls<=options_.maxControlFrames &&
                v.inputBytes<=options_.codec.maxBufferedBytes && v.fragmentedBytes<=options_.codec.maxMessageBytes);
            GAMENET_TEST_ASSERT(v.enqueuedControlBytes==v.admittedControlBytes+v.discardedControlBytes+v.pendingControlBytes && v.admittedControlBytes<=t.controlBytes);
            if(v.phase==Phase::Closed)GAMENET_TEST_ASSERT(r.closed==1 && !v.inputBytes && !v.fragmentedBytes && !v.pendingControls && !v.pendingControlBytes && !v.continuationQueued);
            else GAMENET_TEST_ASSERT(!r.closed);
        } else GAMENET_TEST_ASSERT(r.closed==1);
    }
    void replace() {
        if(auto old=records_[generation_].session.lock()) {(void)old->abandon();check(generation_);}
        session_.reset();GAMENET_TEST_ASSERT(++generation_<records_.size());
        auto& r=records_[generation_];r.transport=std::make_shared<Transport>(loop_);
        const auto id=generation_;
        session_=Session::createWithTransport(r.transport,{
            .handshake=[&,id](const auto&){GAMENET_TEST_ASSERT(!records_[id].closed);return !rejectPolicy_;},
            .ready=[&,id]{auto& r=records_[id];GAMENET_TEST_ASSERT(!r.closed && ++r.ready==1);callback(id);},
            .message=[&,id](auto type,auto bytes){auto& r=records_[id];GAMENET_TEST_ASSERT(!r.closed && r.ready==1 &&
                (type==Opcode::Text || type==Opcode::Binary) && bytes.size()<=64);++r.messages;callback(id);},
            .pong=[&,id](auto){GAMENET_TEST_ASSERT(!records_[id].closed);++records_[id].pongs;callback(id);},
            .closed=[&,id](auto){GAMENET_TEST_ASSERT(++records_[id].closed==1);
                if(callbackMode_==4 && id==generation_)session_.reset();}
        },options_);
        r.session=session_;r.transport->session=session_;
    }
    void feed(std::string_view bytes,unsigned offered=0) {
        // When the caller owns the session, do not hide its last-reference
        // release behind an extra harness lease. Production dispatch must lease.
        const auto queuedLease=session_?std::shared_ptr<Session>{}:records_[generation_].session.lock();
        auto* s=session_?session_.get():queuedLease.get();if(!s)return;
        records_[generation_].offered+=offered;net::Buffer input;input.append(bytes);
        const auto result=s->onMessage(input);
        GAMENET_TEST_ASSERT(result==Operation::NotReady || !input.readableBytes());
    }
    void finish() {
        if(done_)return;done_=true;
        for(std::size_t i=1;i<=generation_;++i) {
            if(auto s=records_[i].session.lock())(void)s->abandon();
            records_[i].transport->discard();check(i);
        }
        session_.reset();loop_.quit();
    }
    void step() {
        if(done_)return;
        if(offset_+3>=input_.size()){finish();return;}
        const unsigned op=input_[offset_]%24,a=input_[offset_+1],b=input_[offset_+2];offset_+=4;
        const auto queuedLease=session_?std::shared_ptr<Session>{}:records_[generation_].session.lock();
        auto* s=session_?session_.get():queuedLease.get();auto t=records_[generation_].transport;
        switch(op) {
        case 0:if(s)(void)s->start();break;
        case 1:{if(s)(void)s->start();const auto split=a%websocketUpgrade.size();
            feed(std::string_view(websocketUpgrade).substr(0,split));feed(std::string_view(websocketUpgrade).substr(split));break;}
        case 2:case 3:feed(websocketClientFrame(op==2?1:2,std::string(a%37,static_cast<char>('a'+b%26))),1);break;
        case 4:feed(websocketClientFrame(2,"a",false)+websocketClientFrame(9,"p")+websocketClientFrame(0,"b"),1);break;
        case 5:case 6:feed(websocketClientFrame(op==5?9:10,std::string(a%33,'p')));break;
        case 7:if(s){const auto before=t->accepted;
            const auto result=s->send(a%3==0?Opcode::Text:a%3==1?Opcode::Binary:Opcode::Ping,std::string(b%37,'o'));
            GAMENET_TEST_ASSERT(result==Operation::Accepted?t->accepted>before:t->accepted==before);}break;
        case 8:t->mode=a%4;break;
        case 9:t->mode=0;t->drain();if(s)s->onWritable();break;
        case 10:if(s)(void)s->close(a%2?1000:1005);break;
        case 11:t->down(a%2);break;
        case 12:if(s)(void)s->abort();break;
        case 13:if(s)(void)s->abandon();break;
        case 14:replace();break;
        case 15:callbackMode_=a%8;break;
        case 16:if(a&128)feed(std::string(1025,'z'));
            else {auto wire=websocketClientFrame(1,"bad");wire[a%wire.size()]=static_cast<char>(b);feed(wire,1);}break;
        case 17:if(a%2)for(unsigned n=0;n<8;++n)(void)loop_.executor().post([]{});
            feed(websocketClientFrame(1,"a")+websocketClientFrame(1,"b")+websocketClientFrame(1,"c"),3);break;
        case 18:if(generation_>1)if(auto old=records_[a%(generation_-1)+1].session.lock()){
            const auto before=records_[generation_].messages;old->onWritable();old->onDisconnect();
            net::Buffer input;input.append(websocketClientFrame(1,"old"));(void)old->onMessage(input);
            GAMENET_TEST_ASSERT(before==records_[generation_].messages);}break;
        case 19:t->phase=a%2?websocket::WebSocketTransportState::Ready:websocket::WebSocketTransportState::NotReady;if(s)s->onWritable();break;
        case 20:session_.reset();break;
        case 21:rejectPolicy_=a%2;break;
        case 22:t->mode=1;for(unsigned n=0;n<5;++n)feed(websocketClientFrame(9,"queued"));break;
        case 23:if(s)(void)s->start();feed(websocketUpgrade+websocketClientFrame(1,"pipeline"),1);break;
        }
        for(std::size_t i=1;i<=generation_;++i)check(i);
        if(loop_.executor().post([this]{step();})!=net::PostResult::Accepted)finish();
    }
public:
    void run(std::span<const std::uint8_t> bytes) {
        options_.codec={.maxFrameBytes=32,.maxMessageBytes=64,.maxBufferedBytes=1024,.maxFramesPerPush=bytes.empty() || !(bytes[0]&32)?4u:1u};
        options_.maxHeaderBytes=512;options_.maxHeaderFields=16;options_.maxControlFrames=2;options_.maxControlBytes=512;
        options_.handshakeTimeout=options_.closeTimeout=options_.retryDelay=std::chrono::hours(1);
        input_.assign(bytes.begin(),bytes.begin()+std::min<std::size_t>(bytes.size(),512));
        replace();(void)session_->start();feed(websocketUpgrade);check(generation_);
        GAMENET_TEST_ASSERT(loop_.executor().post([this]{step();})==net::PostResult::Accepted);
        loop_.wakeup(); // owner pre-loop posts do not signal the poller themselves
        loop_.loop();
        for(std::size_t i=1;i<=generation_;++i) {check(i);GAMENET_TEST_ASSERT(records_[i].session.expired() && records_[i].closed==1 && !records_[i].transport->bytes);}
    }
};
inline void runWebSocketStateSequence(std::span<const std::uint8_t> input){WebSocketStateSequence sequence;sequence.run(input);}
inline void verifyWebSocketStateSeeds() {
    const std::vector<std::uint8_t> actions{
        8,1,0,0, 7,0,8,0, 5,0,0,0, 9,0,0,0, 7,1,8,0, 4,0,0,0,
        15,5,0,0, 2,4,0,0, 14,0,0,0, 1,7,0,0, 15,4,0,0, 2,3,0,0,
        14,0,0,0, 1,0,0,0, 8,2,0,0, 5,1,0,0, 14,0,0,0, 23,0,0,0, 10,1,0,0, 11,1,0,0};
    runWebSocketStateSequence(actions);
    for(std::uint32_t seed=1;seed<=32;++seed){std::array<std::uint8_t,512> input{};auto state=seed;
        for(auto& byte:input){state=state*1664525u+1013904223u;byte=static_cast<std::uint8_t>(state>>24);}runWebSocketStateSequence(input);}
}
} // namespace gamenet::test
