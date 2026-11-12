// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket/WebSocketCodec.h"
#include "support/WebSocketWire.h"
#include "support/TestAssert.h"
#include "support/WebSocketCodecOracle.h"
using namespace gamenet::websocket;
using namespace gamenet::test;
int main() {
    for (const auto& wire : {websocketUpgrade,websocketClientFrame(1,"hello"),websocketClientFrame(1,"\xe2",false)+websocketClientFrame(0,"\x82\xac"),std::string(300,'x')})
        verifyWebSocketBytes(std::span(reinterpret_cast<const std::uint8_t*>(wire.data()),wire.size()));
    const auto upgrade = parseWebSocketUpgrade(websocketUpgrade + "frames");
    GAMENET_TEST_ASSERT(upgrade.httpStatus == 101 && upgrade.consumedBytes == websocketUpgrade.size() &&
        upgrade.request.target == "/echo" && upgrade.response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    for (std::size_t n = 0; n < websocketUpgrade.size(); ++n)
        GAMENET_TEST_ASSERT(parseWebSocketUpgrade(std::string_view(websocketUpgrade).substr(0,n)).httpStatus == 0);
    for (const auto extra : {"Host: other\r\n", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", "Transfer-Encoding: chunked\r\n",
        "Content-Length: 1\r\n", "Expect: 100-continue\r\n", "Bad : value\r\n", " folded: value\r\n", "X: bad\nvalue\r\n"}) {
        auto bad = websocketUpgrade; bad.insert(bad.size()-2, extra);
        GAMENET_TEST_ASSERT(parseWebSocketUpgrade(bad).httpStatus == 400);
    }
    auto version = websocketUpgrade; version.replace(version.find("Version: 13"), 11, "Version: 12");
    GAMENET_TEST_ASSERT(parseWebSocketUpgrade(version).httpStatus == 426);
    GAMENET_TEST_ASSERT(parseWebSocketUpgrade(websocketUpgrade, 32).httpStatus == 431);
    GAMENET_TEST_ASSERT(parseWebSocketUpgrade(websocketUpgrade, 8192, 2).httpStatus == 431);
    for (const auto text : {std::string("ASCII"), std::string("\xe2\x82\xac"), std::string("\xf0\x9f\x98\x80")})
        GAMENET_TEST_ASSERT(WebSocketCodec::validUtf8(text));
    for (const auto text : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"})
        GAMENET_TEST_ASSERT(!WebSocketCodec::validUtf8(text));
    const auto hello = websocketClientFrame(1,"Hello");
    for (std::size_t split = 0; split <= hello.size(); ++split) {
        WebSocketCodec c; auto a = c.push(std::string_view(hello).substr(0,split)); auto b = c.push(std::string_view(hello).substr(split));
        GAMENET_TEST_ASSERT(!a.errorCode && !b.errorCode && a.events.size()+b.events.size()==1);
        GAMENET_TEST_ASSERT((a.events.empty()?b.events[0]:a.events[0]).payload=="Hello");
    }
    WebSocketCodec c;
    auto result=c.push(websocketClientFrame(1,"\xe2",false)+websocketClientFrame(9,"ping")+websocketClientFrame(0,"\x82\xac"));
    GAMENET_TEST_ASSERT(!result.errorCode && result.events.size()==2 && result.events[0].opcode==WebSocketOpcode::Ping && result.events[1].payload=="\xe2\x82\xac");
    for (const auto& bad : {websocketClientFrame(1,"x",true,false),websocketClientFrame(0,"x"),websocketClientFrame(3,""),
        websocketClientFrame(9,"",false),websocketClientFrame(9,std::string(126,'x')),websocketClientFrame(8,std::string(1,'x')),
        websocketClientFrame(8,std::string("\x03\xed",2)),websocketClientFrame(1,"\xc0\x80")}) {
        c.reset(); GAMENET_TEST_ASSERT(c.push(bad).errorCode); GAMENET_TEST_ASSERT(c.push(hello).errorCode);
    }
    c.reset(); auto nonminimal=std::string("\x81\xfe\0\1",4)+std::string(4,'\0')+"x";
    GAMENET_TEST_ASSERT(c.push(nonminimal).errorCode==1002);
    c.reset(); auto rsv=hello; rsv[0]=static_cast<char>(0xc1); GAMENET_TEST_ASSERT(c.push(rsv).errorCode==1002);
    WebSocketCodec budget({.maxFrameBytes=128,.maxMessageBytes=256,.maxBufferedBytes=512,.maxFramesPerPush=1});
    auto batch=budget.push(hello+hello+hello); GAMENET_TEST_ASSERT(batch.events.size()==1 && batch.needsContinuation);
    GAMENET_TEST_ASSERT(budget.push({}).events.size()==1 && budget.push({}).events.size()==1 && !budget.bufferedBytes());
    WebSocketCodec limit({.maxFrameBytes=8,.maxMessageBytes=10,.maxBufferedBytes=32});
    GAMENET_TEST_ASSERT(limit.push(websocketClientFrame(2,"123456",false)+websocketClientFrame(0,"123456")).errorCode==1009);
    limit.reset(); GAMENET_TEST_ASSERT(limit.push(std::string(33,'x')).errorCode==1009 && !limit.bufferedBytes());
    WebSocketCodec large({.maxFrameBytes=70000,.maxMessageBytes=70000,.maxBufferedBytes=80000});
    const auto payload=std::string(65536,'b'); GAMENET_TEST_ASSERT(large.push(websocketClientFrame(2,payload)).events[0].payload==payload);
    GAMENET_TEST_ASSERT(WebSocketCodec::encodeServer(WebSocketOpcode::Text,"Hello")==std::optional(std::string("\x81\x05Hello",7)));
    GAMENET_TEST_ASSERT(!WebSocketCodec::encodeServer(WebSocketOpcode::Text,"\xc0\x80"));
    GAMENET_TEST_ASSERT(!WebSocketCodec::encodeServer(WebSocketOpcode::Close,std::string("\x03\xed",2)));
    GAMENET_TEST_ASSERT(!WebSocketCodec::encodeServer(WebSocketOpcode::Ping,std::string(126,'x')));
}
