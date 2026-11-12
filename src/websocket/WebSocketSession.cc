// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket/WebSocketSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpConnection.h"
#include <deque>
#include <stdexcept>
#include <utility>

namespace gamenet::websocket {
using namespace std::chrono_literals;
struct WebSocketSession::Impl {
    enum class ControlKind { Upgrade, Reject, Pong, Close };
    struct Control { ControlKind kind; std::string wire; };
    std::weak_ptr<net::TcpConnection> connection;
    std::weak_ptr<WebSocketSession> self;
    net::EventLoop* loop;
    WebSocketCallbacks callbacks;
    WebSocketOptions options;
    WebSocketCodec codec;
    WebSocketSnapshot stats;
    std::string input;
    std::deque<Control> controls;
    net::TimerId deadline, retry;
    bool driving{}, shutdownStarted{}, protocolFailure{}, transportClean{};

    Impl(const net::TcpConnectionPtr& c,WebSocketCallbacks cb,WebSocketOptions opts)
        :connection(c),loop(c->getLoop()),callbacks(std::move(cb)),options(opts),codec(opts.codec) {
        if (opts.maxHeaderBytes<128 || opts.maxHeaderBytes>opts.codec.maxBufferedBytes || !opts.maxHeaderFields ||
            !opts.maxControlFrames || opts.maxControlBytes<256 || opts.handshakeTimeout<=0ms || opts.closeTimeout<=0ms || opts.retryDelay<=0ms)
            throw std::invalid_argument("invalid WebSocket session bounds");
    }
    bool ended() const {return stats.phase==WebSocketPhase::Closed;}
    void discardControls() {
        stats.discardedControlBytes+=stats.pendingControlBytes;
        stats.pendingControlBytes=0;controls.clear();
    }
    void finish(WebSocketError error,bool force) noexcept {
        if (ended()) return;
        stats.phase=WebSocketPhase::Closed;stats.terminal=error;
        (void)loop->tryCancel(deadline);(void)loop->tryCancel(retry);deadline={};retry={};
        discardControls();input.clear();codec.reset();
        if (force) if (auto c=connection.lock()) c->forceClose();
        try {if (callbacks.closed) callbacks.closed(error);} catch (...) {++stats.callbackExceptions;}
    }
    void armDeadline(std::chrono::milliseconds timeout,WebSocketError error) {
        (void)loop->tryCancel(deadline);deadline={};const auto weak=self;
        try {
            const auto scheduled=loop->tryRunAfter(timeout,[weak,error] {
                if (auto session=weak.lock()) {
                    auto& s=*session->impl_;
                    if (error==WebSocketError::CloseTimeout) s.stats.closeTimedOut=true;
                    s.finish(s.stats.terminal==WebSocketError::None?error:s.stats.terminal,true);
                }
            });
            if (scheduled.result==net::PostResult::Accepted) {deadline=scheduled.timerId;return;}
        } catch (...) {}
        finish(WebSocketError::SchedulingRejected,true);
    }
    void armRetry() {
        if (ended() || stats.transportDown || retry.valid() || controls.empty()) return;
        const auto weak=self;
        try {
            const auto scheduled=loop->tryRunAfter(options.retryDelay,[weak] {
                if (auto session=weak.lock()) {session->impl_->retry={};session->drive();}
            });
            if (scheduled.result==net::PostResult::Accepted) {retry=scheduled.timerId;return;}
        } catch (...) {}
        finish(WebSocketError::SchedulingRejected,true);
    }
    bool enqueue(ControlKind kind,std::string wire) {
        if (controls.size()>=options.maxControlFrames || wire.size()>options.maxControlBytes-stats.pendingControlBytes) {
            finish(WebSocketError::Backpressure,true);return false;
        }
        const auto bytes=wire.size();controls.push_back({kind,std::move(wire)});
        stats.pendingControlBytes+=bytes;stats.enqueuedControlBytes+=bytes;return true;
    }
    void shutdownTcp() {
        if (shutdownStarted || ended() || stats.transportDown) return;
        shutdownStarted=true;
        if (auto c=connection.lock()) {
            if (c->tryShutdown()==net::PostResult::Accepted) return;
        }
        finish(WebSocketError::SchedulingRejected,true);
    }
    void beginClose(std::string payload,WebSocketError error=WebSocketError::None,bool failure=false) {
        if (ended()) return;
        if (error!=WebSocketError::None && stats.terminal==WebSocketError::None) stats.terminal=error;
        protocolFailure|=failure;
        if (stats.phase!=WebSocketPhase::Closing) {
            stats.phase=WebSocketPhase::Closing;discardControls();
            armDeadline(options.closeTimeout,WebSocketError::CloseTimeout);
            if (!ended()) (void)enqueue(ControlKind::Close,*WebSocketCodec::encodeServer(WebSocketOpcode::Close,payload));
        }
    }
    void protocolError(std::uint16_t code) {
        const std::string payload{static_cast<char>(code>>8),static_cast<char>(code)};
        beginClose(payload,code==1007?WebSocketError::InvalidUtf8:code==1009?WebSocketError::MessageTooLarge:WebSocketError::ProtocolError,true);
    }
    void reject(unsigned code,WebSocketError error=WebSocketError::HandshakeRejected) {
        stats.phase=WebSocketPhase::Rejecting;stats.httpStatus=code;stats.terminal=error;input.clear();codec.reset();discardControls();
        armDeadline(options.closeTimeout,WebSocketError::CloseTimeout);
        if (ended()) return;
        auto response="HTTP/1.1 "+std::to_string(code)+" Rejected\r\nConnection: close\r\nContent-Length: 0\r\n";
        if (code==426) response+="Sec-WebSocket-Version: 13\r\n";
        (void)enqueue(ControlKind::Reject,response+"\r\n");
    }
    void flush() {
        while (!ended() && !stats.transportDown && !controls.empty()) {
            const auto c=connection.lock();
            if (!c || !c->connected()) {stats.transportDown=true;break;}
            if (c->trySend(controls.front().wire)!=net::TcpSendResult::Accepted) {++stats.controlRetries;break;}
            const auto kind=controls.front().kind;const auto bytes=controls.front().wire.size();
            controls.pop_front();stats.pendingControlBytes-=bytes;stats.admittedControlBytes+=bytes;
            if (kind==ControlKind::Upgrade) {
                stats.phase=WebSocketPhase::Open;(void)loop->tryCancel(deadline);deadline={};
                try {if (callbacks.ready) callbacks.ready();}
                catch (...) {++stats.callbackExceptions;beginClose(std::string("\x03\xf3",2),WebSocketError::CallbackException,true);}
            } else if (kind==ControlKind::Close) stats.sentClose=true;
            else if (kind==ControlKind::Reject) shutdownTcp();
        }
        if (!ended() && stats.sentClose && (stats.receivedClose || protocolFailure)) shutdownTcp();
        if (controls.empty()) {(void)loop->tryCancel(retry);retry={};} else armRetry();
    }
    void postContinuation() {
        if (ended() || stats.continuationQueued) return;
        const auto session=self.lock();
        if (loop->executor().post([session] {session->impl_->stats.continuationQueued=false;session->drive();})==net::PostResult::Accepted)
            stats.continuationQueued=true;
        else finish(WebSocketError::SchedulingRejected,true);
    }
    void event(WebSocketEvent& event) {
        if (event.opcode==WebSocketOpcode::Close) {
            stats.receivedClose=true;
            if (stats.phase==WebSocketPhase::Open) beginClose(event.payload);
        } else if (stats.phase==WebSocketPhase::Open) {
            if (event.opcode==WebSocketOpcode::Ping) {
                if (!stats.transportDown) (void)enqueue(ControlKind::Pong,*WebSocketCodec::encodeServer(WebSocketOpcode::Pong,event.payload));
            } else try {
                if (event.opcode==WebSocketOpcode::Pong) {if (callbacks.pong) callbacks.pong(event.payload);}
                else {++stats.receivedMessages;if (callbacks.message) callbacks.message(event.opcode,event.payload);}
            } catch (...) {++stats.callbackExceptions;beginClose(std::string("\x03\xf3",2),WebSocketError::CallbackException,true);}
        }
        flush();
    }
};
WebSocketSession::WebSocketSession(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
std::shared_ptr<WebSocketSession> WebSocketSession::create(const net::TcpConnectionPtr& connection,
    WebSocketCallbacks callbacks,WebSocketOptions options) {
    if (!connection) throw std::invalid_argument("WebSocket requires TCP");
    connection->getLoop()->assertInLoopThread();
    auto session=std::shared_ptr<WebSocketSession>(new WebSocketSession(std::make_unique<Impl>(connection,std::move(callbacks),options)));
    session->impl_->self=session;return session;
}
WebSocketSession::~WebSocketSession() {
    impl_->loop->assertInLoopThread();
    if (!impl_->ended()) {impl_->finish(WebSocketError::Aborted,false);if (auto c=impl_->connection.lock()) c->shutdown();}
}
net::EventLoopExecutor WebSocketSession::ownerExecutor() const noexcept {return impl_->loop->executor();}
WebSocketSnapshot WebSocketSession::snapshot() const {
    impl_->loop->assertInLoopThread();auto s=impl_->stats;
    s.inputBytes=impl_->input.size()+impl_->codec.bufferedBytes();s.fragmentedBytes=impl_->codec.fragmentedBytes();
    s.pendingControls=impl_->controls.size();return s;
}
WebSocketOperation WebSocketSession::start() {
    auto& s=*impl_;if (!s.loop->isInLoopThread()) return WebSocketOperation::WrongThread;
    const auto lease=shared_from_this();if (s.ended()) return WebSocketOperation::Closed;
    if (s.stats.phase!=WebSocketPhase::Created) return WebSocketOperation::Accepted;
    if (s.loop->phase()!=net::EventLoopPhase::Running) return WebSocketOperation::OwnerUnavailable;
    s.stats.phase=WebSocketPhase::Handshaking;s.armDeadline(s.options.handshakeTimeout,WebSocketError::HandshakeTimeout);
    return s.ended()?WebSocketOperation::OwnerUnavailable:WebSocketOperation::Accepted;
}
WebSocketOperation WebSocketSession::send(WebSocketOpcode type,std::string_view payload) {
    auto& s=*impl_;if (!s.loop->isInLoopThread()) return WebSocketOperation::WrongThread;
    const auto lease=shared_from_this();
    if (s.ended() || s.stats.transportDown || s.stats.phase==WebSocketPhase::Closing || s.stats.phase==WebSocketPhase::Rejecting) return WebSocketOperation::Closed;
    if (s.stats.phase!=WebSocketPhase::Open) return WebSocketOperation::NotReady;
    if (type!=WebSocketOpcode::Text && type!=WebSocketOpcode::Binary && type!=WebSocketOpcode::Ping) return WebSocketOperation::InvalidArgument;
    const auto wire=WebSocketCodec::encodeServer(type,payload,s.options.codec.maxFrameBytes);
    if (!wire) return WebSocketOperation::InvalidArgument;
    if (s.controls.empty()) if (auto c=s.connection.lock())
        if (c->trySend(*wire)==net::TcpSendResult::Accepted) {++s.stats.acceptedSends;return WebSocketOperation::Accepted;}
    ++s.stats.rejectedSends;return WebSocketOperation::Overloaded;
}
WebSocketOperation WebSocketSession::close(std::uint16_t code,std::string_view reason) {
    auto& s=*impl_;if (!s.loop->isInLoopThread()) return WebSocketOperation::WrongThread;
    const auto lease=shared_from_this();if (s.ended() || s.stats.transportDown) return WebSocketOperation::Closed;
    if (reason.size()>123) return WebSocketOperation::InvalidArgument;
    const auto payload=std::string{static_cast<char>(code>>8),static_cast<char>(code)}+std::string(reason);
    if (!WebSocketCodec::encodeServer(WebSocketOpcode::Close,payload)) return WebSocketOperation::InvalidArgument;
    if (s.stats.phase==WebSocketPhase::Open || s.stats.phase==WebSocketPhase::Closing) {s.beginClose(payload);drive();}
    else s.finish(WebSocketError::Aborted,true);
    return WebSocketOperation::Accepted;
}
WebSocketOperation WebSocketSession::abort() {
    if (!impl_->loop->isInLoopThread()) return WebSocketOperation::WrongThread;
    const auto lease=shared_from_this();impl_->finish(WebSocketError::Aborted,true);return WebSocketOperation::Accepted;
}
WebSocketOperation WebSocketSession::onMessage(net::Buffer& input) {
    auto& s=*impl_;if (!s.loop->isInLoopThread()) return WebSocketOperation::WrongThread;
    const auto lease=shared_from_this();
    if (s.driving) {++s.stats.reentrantMessages;return WebSocketOperation::NotReady;}
    if (s.ended()) {input.retrieveAll();return WebSocketOperation::Closed;}
    if (s.stats.phase==WebSocketPhase::Created) return WebSocketOperation::NotReady;
    const auto retained=s.input.size()+s.codec.bufferedBytes();
    if (input.readableBytes()>s.options.codec.maxBufferedBytes-retained) {
        input.retrieveAll();
        if (s.stats.phase==WebSocketPhase::Handshaking) s.reject(431);else s.protocolError(1009);
    } else {s.input.append(input.peek(),input.readableBytes());input.retrieveAll();}
    drive();return WebSocketOperation::Accepted;
}
void WebSocketSession::onWritable() {impl_->loop->assertInLoopThread();const auto lease=shared_from_this();drive();}
void WebSocketSession::onDisconnect() {
    impl_->loop->assertInLoopThread();const auto lease=shared_from_this();
    if (const auto c=impl_->connection.lock()) {
        const auto info=c->closeInfo();
        impl_->transportClean=info && info->nativeError==0 &&
            (info->reason==net::TcpConnectionCloseReason::PeerEof || info->reason==net::TcpConnectionCloseReason::GracefulShutdown);
    }
    impl_->stats.transportDown=true;(void)impl_->loop->tryCancel(impl_->retry);impl_->retry={};drive();
}
void WebSocketSession::drive() {
    auto& s=*impl_;if (s.driving || s.ended()) return;
    const auto lease=shared_from_this();s.driving=true;
    try {
        if (s.stats.phase==WebSocketPhase::Handshaking) {
            auto upgrade=parseWebSocketUpgrade(s.input,s.options.maxHeaderBytes,s.options.maxHeaderFields);
            if (upgrade.httpStatus) {
                if (upgrade.httpStatus!=101) s.reject(upgrade.httpStatus);
                else {
                    bool accepted=true;
                    try {if (s.callbacks.handshake) accepted=s.callbacks.handshake(upgrade.request);}
                    catch (...) {++s.stats.callbackExceptions;accepted=false;}
                    if (s.stats.phase==WebSocketPhase::Handshaking) {
                        if (!accepted) s.reject(403);
                        else {s.input.erase(0,upgrade.consumedBytes);s.stats.httpStatus=101;s.stats.phase=WebSocketPhase::Upgrading;
                            (void)s.enqueue(Impl::ControlKind::Upgrade,std::move(upgrade.response));}
                    }
                }
            }
        }
        s.flush();
        if (!s.ended() && (s.stats.phase==WebSocketPhase::Open || s.stats.phase==WebSocketPhase::Closing) && !s.protocolFailure) {
            const auto bytes=std::exchange(s.input,{});auto decoded=s.codec.push(bytes);
            for (auto& event:decoded.events) {if (s.ended()) break;s.event(event);}
            if (!s.ended() && decoded.errorCode) {s.protocolError(decoded.errorCode);s.flush();}
            else if (!s.ended() && decoded.needsContinuation) s.postContinuation();
        }
        if (!s.ended() && s.stats.transportDown && !s.stats.continuationQueued) {
            const auto reason=s.stats.terminal!=WebSocketError::None?s.stats.terminal:
                s.transportClean && s.stats.sentClose && s.stats.receivedClose && s.controls.empty()?WebSocketError::Graceful:WebSocketError::Truncated;
            s.finish(reason,false);
        }
    } catch (...) {s.finish(WebSocketError::TransportError,true);}
    s.driving=false;
}
} // namespace gamenet::websocket
