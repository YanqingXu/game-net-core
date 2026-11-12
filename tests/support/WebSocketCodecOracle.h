// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/websocket/WebSocketCodec.h"
#include "support/WebSocketWire.h"
#include "support/TestAssert.h"
#include <span>
#include <algorithm>
#include <tuple>
namespace gamenet::test {
inline void verifyWebSocketBytes(std::span<const std::uint8_t> bytes) {
    using namespace websocket;
    const auto size=std::min<std::size_t>(bytes.size(),2048);
    const std::string_view input(reinterpret_cast<const char*>(bytes.data()),size);
    auto decode=[&](std::size_t chunk){
        WebSocketCodec codec({.maxFrameBytes=256,.maxMessageBytes=512,.maxBufferedBytes=4096,.maxFramesPerPush=2});
        std::vector<std::pair<unsigned,std::string>> events;std::uint16_t error=0;
        for(std::size_t n=0;n<input.size() && !error;){
            const auto count=std::min(chunk,input.size()-n);auto result=codec.push(input.substr(n,count));n+=count;
            unsigned drains=0;
            while(true){
                for(const auto& event:result.events)events.emplace_back(static_cast<unsigned>(event.opcode),event.payload);
                error=result.errorCode;
                GAMENET_TEST_ASSERT(codec.bufferedBytes()<=4096 && codec.fragmentedBytes()<=512);
                if(error || !result.needsContinuation)break;
                GAMENET_TEST_ASSERT(++drains<1024);result=codec.push({});
            }
        }
        return std::tuple{events,error,codec.bufferedBytes(),codec.fragmentedBytes()};
    };
    GAMENET_TEST_ASSERT(decode(4096)==decode(size?1+bytes[0]%31:1));
    const auto header=parseWebSocketUpgrade(input,1024,16);
    GAMENET_TEST_ASSERT(header.response.size()<512 && (!header.httpStatus || header.httpStatus==101 || header.httpStatus==400 || header.httpStatus==426 || header.httpStatus==431));
    if(header.httpStatus==101)GAMENET_TEST_ASSERT(header.consumedBytes<=input.size() && header.response.starts_with("HTTP/1.1 101"));
    const auto payload=input.substr(0,128);
    const auto encoded=WebSocketCodec::encodeServer(WebSocketOpcode::Binary,payload,128);
    GAMENET_TEST_ASSERT(encoded && (static_cast<unsigned char>((*encoded)[0])==0x82) && !(static_cast<unsigned char>((*encoded)[1])&128));
    WebSocketCodec roundtrip;
    const auto result=roundtrip.push(websocketClientFrame(2,payload));
    GAMENET_TEST_ASSERT(!result.errorCode && result.events.size()==1 && result.events[0].payload==payload);
}
} // namespace gamenet::test
