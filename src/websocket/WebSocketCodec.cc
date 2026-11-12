// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/websocket/WebSocketCodec.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>

namespace gamenet::websocket {
namespace {
bool token(std::string_view value) {
    if (value.empty()) return false;
    for (const unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos)) return false;
    return true;
}
std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a'-'A'));
    return result;
}
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front()==' ' || value.front()=='\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back()==' ' || value.back()=='\t')) value.remove_suffix(1);
    return value;
}
bool tokens(std::string_view value, std::string_view wanted, bool& found, bool upgrade = false) {
    while (true) {
        const auto comma=value.find(','); const auto item=trim(value.substr(0,comma));
        const auto slash=item.find('/');
        if (!token(item) && !(upgrade && slash!=std::string_view::npos && token(item.substr(0,slash)) && token(item.substr(slash+1)))) return false;
        if (lower(item)==wanted) found=true;
        if (comma==std::string_view::npos) return true;
        value.remove_prefix(comma+1);
    }
}
constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
bool keyValid(std::string_view key) {
    if (key.size()!=24 || key.substr(22)!="==") return false;
    for (unsigned n=0;n<22;++n) if (alphabet.find(key[n])==std::string_view::npos) return false;
    return (alphabet.find(key[21]) & 15)==0;
}
// Fixed-size SHA-1 input: canonical 24-byte nonce text plus the 36-byte RFC GUID.
// This handshake digest is not used for signatures, password hashing or TLS.
std::string acceptKey(std::string_view key) {
    const auto input=std::string(key)+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<std::uint8_t,128> padded{};
    std::copy(input.begin(),input.end(),padded.begin()); padded[input.size()]=128;
    const auto bits=std::uint64_t(input.size())*8;
    for (unsigned n=0;n<8;++n) padded[127-n]=static_cast<std::uint8_t>(bits>>(8*n));
    std::array<std::uint32_t,5> h{0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    for (unsigned block=0;block<2;++block) {
        std::array<std::uint32_t,80> w{};
        for (unsigned n=0;n<16;++n) for (unsigned j=0;j<4;++j) w[n]=(w[n]<<8)|padded[block*64+n*4+j];
        for (unsigned n=16;n<80;++n) w[n]=std::rotl(w[n-3]^w[n-8]^w[n-14]^w[n-16],1);
        auto [a,b,c,d,e]=h;
        for (unsigned n=0;n<80;++n) {
            const auto f=n<20 ? (b&c)|(~b&d) : n<40 ? b^c^d : n<60 ? (b&c)|(b&d)|(c&d) : b^c^d;
            const auto k=n<20?0x5a827999u:n<40?0x6ed9eba1u:n<60?0x8f1bbcdcu:0xca62c1d6u;
            const auto next=std::rotl(a,5)+f+e+k+w[n]; e=d;d=c;c=std::rotl(b,30);b=a;a=next;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    std::array<unsigned char,20> digest{};
    for (unsigned n=0;n<20;++n) digest[n]=static_cast<unsigned char>(h[n/4]>>(24-8*(n%4)));
    std::string result;
    for (unsigned n=0;n<20;n+=3) {
        const auto value=(unsigned(digest[n])<<16)|(n+1<20?unsigned(digest[n+1])<<8:0)|(n+2<20?digest[n+2]:0);
        result+=alphabet[(value>>18)&63];result+=alphabet[(value>>12)&63];
        result+=n+1<20?alphabet[(value>>6)&63]:'=';result+=n+2<20?alphabet[value&63]:'=';
    }
    return result;
}
bool closeValid(std::string_view payload) {
    if (payload.empty()) return true;
    if (payload.size()<2 || payload.size()>125) return false;
    const auto code=(unsigned(static_cast<unsigned char>(payload[0]))<<8)|static_cast<unsigned char>(payload[1]);
    const bool legal=(code>=1000 && code<=1014 && code!=1004 && code!=1005 && code!=1006) || (code>=3000 && code<=4999);
    return legal && WebSocketCodec::validUtf8(payload.substr(2));
}
WebSocketUpgrade rejection(unsigned code) {
    const auto reason=code==426?"Upgrade Required":code==431?"Request Header Fields Too Large":"Bad Request";
    return {code,0,{},"HTTP/1.1 "+std::to_string(code)+" "+reason+"\r\nConnection: close\r\nContent-Length: 0\r\n"+
        (code==426?"Sec-WebSocket-Version: 13\r\n":"")+"\r\n"};
}
} // namespace

WebSocketUpgrade parseWebSocketUpgrade(std::string_view input, std::size_t maxBytes, std::size_t maxFields) {
    const auto end=input.find("\r\n\r\n");
    const auto size=end==std::string_view::npos?input.size():end+4;
    if (size>maxBytes) return rejection(431);
    auto header=input.substr(0,size);
    for (std::size_t n=0;n<header.size();++n) {
        const auto c=static_cast<unsigned char>(header[n]);
        if ((c=='\n' && (!n || header[n-1]!='\r')) || (c=='\r' && n+1<header.size() && header[n+1]!='\n') ||
            (c<32 && c!='\r' && c!='\n' && c!='\t') || c==127) return rejection(400);
    }
    if (end==std::string_view::npos) return {};
    const auto lineEnd=header.find("\r\n");const auto first=header.substr(0,lineEnd);
    if (!first.starts_with("GET ") || !first.ends_with(" HTTP/1.1")) return rejection(400);
    const auto target=first.substr(4,first.size()-4-9);
    if (target.empty() || target[0]!='/' || target.size()>2048) return rejection(400);
    for (const unsigned char c:target) if (c<=32 || c>=127 || c=='#') return rejection(400);
    std::string_view host,key,version,origin;
    bool haveHost=false,haveKey=false,haveVersion=false,haveOrigin=false,haveLength=false,haveProtocol=false;
    bool upgrade=false,connection=false;std::size_t count=0,offset=lineEnd+2;
    while (offset<end) {
        const auto next=header.find("\r\n",offset);const auto line=header.substr(offset,next-offset);offset=next+2;
        if (++count>maxFields) return rejection(431);
        const auto colon=line.find(':');
        if (colon==std::string_view::npos || !token(line.substr(0,colon))) return rejection(400);
        const auto name=lower(line.substr(0,colon));const auto value=trim(line.substr(colon+1));
        if (name=="host") { if (haveHost || value.empty()) return rejection(400);haveHost=true;host=value; }
        else if (name=="sec-websocket-key") { if (haveKey) return rejection(400);haveKey=true;key=value; }
        else if (name=="sec-websocket-version") { if (haveVersion) return rejection(400);haveVersion=true;version=value; }
        else if (name=="origin") { if (haveOrigin) return rejection(400);haveOrigin=true;origin=value; }
        else if (name=="upgrade") { if (!tokens(value,"websocket",upgrade,true)) return rejection(400); }
        else if (name=="connection") { if (!tokens(value,"upgrade",connection)) return rejection(400); }
        else if (name=="transfer-encoding" || name=="expect") return rejection(400);
        else if (name=="content-length") { if (haveLength || value!="0") return rejection(400);haveLength=true; }
        else if (name=="sec-websocket-protocol") { bool ignored=false;if (haveProtocol || !tokens(value,"",ignored)) return rejection(400);haveProtocol=true; }
    }
    if (!haveHost || !haveKey || !haveVersion || !upgrade || !connection || !keyValid(key)) return rejection(400);
    for (const unsigned char c:host) if (c<=32 || c>=127 || std::string_view("/@#?\\\"<>").find(static_cast<char>(c))!=std::string_view::npos) return rejection(400);
    if (version!="13") return rejection(426);
    return {101,size,{std::string(target),std::string(host),std::string(origin)},
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+acceptKey(key)+"\r\n\r\n"};
}

bool WebSocketCodec::validUtf8(std::string_view bytes) noexcept {
    std::uint32_t value=0,minimum=0;unsigned remaining=0;
    for (const unsigned char c:bytes) {
        if (!remaining) {
            if (c<128) continue;
            if (c>=0xc2 && c<=0xdf) {value=c&31;remaining=1;minimum=0x80;}
            else if (c>=0xe0 && c<=0xef) {value=c&15;remaining=2;minimum=0x800;}
            else if (c>=0xf0 && c<=0xf4) {value=c&7;remaining=3;minimum=0x10000;}
            else return false;
        } else {
            if ((c&0xc0)!=0x80) return false;
            value=(value<<6)|(c&63);
            if (!--remaining && (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))) return false;
        }
    }
    return remaining==0;
}
WebSocketCodec::WebSocketCodec(WebSocketCodecOptions options):options_(options) {
    if (!options.maxFrameBytes || !options.maxMessageBytes || !options.maxFramesPerPush ||
        options.maxFrameBytes>(std::numeric_limits<std::size_t>::max)()-14 || options.maxBufferedBytes<options.maxFrameBytes+14)
        throw std::invalid_argument("invalid WebSocket codec bounds");
}
void WebSocketCodec::reset() noexcept {input_.clear();message_.clear();fault_=0;fragmented_=WebSocketOpcode::Continuation;}
std::size_t WebSocketCodec::bufferedBytes() const noexcept {return input_.size();}
std::size_t WebSocketCodec::fragmentedBytes() const noexcept {return message_.size();}
WebSocketDecodeResult WebSocketCodec::push(std::string_view bytes) {
    WebSocketDecodeResult result;
    auto fail=[&](std::uint16_t code) {fault_=code;result.errorCode=code;input_.clear();message_.clear();fragmented_=WebSocketOpcode::Continuation;};
    if (fault_) {result.errorCode=fault_;return result;}
    if (bytes.size()>options_.maxBufferedBytes-input_.size()) {fail(1009);return result;}
    input_.append(bytes);std::size_t offset=0;
    while (input_.size()-offset>=2 && result.frames<options_.maxFramesPerPush) {
        const auto* data=reinterpret_cast<const unsigned char*>(input_.data()+offset);
        const auto opcode=static_cast<WebSocketOpcode>(data[0]&15);const bool fin=(data[0]&128)!=0;
        const bool control=(data[0]&8)!=0;std::uint64_t length=data[1]&127;std::size_t header=2;
        if ((data[0]&0x70) || !(data[1]&128) || !((data[0]&15)<=2 || opcode==WebSocketOpcode::Close || opcode==WebSocketOpcode::Ping || opcode==WebSocketOpcode::Pong) ||
            (control && (!fin || length>125))) {fail(1002);return result;}
        if (length>=126) {
            const unsigned width=length==126?2:8;
            if (input_.size()-offset<header+width) break;
            const bool wide=width==8;length=0;
            for (unsigned n=0;n<width;++n) length=(length<<8)|data[header++];
            if ((wide && (data[2]&128)) || length<(wide?65536u:126u)) {fail(1002);return result;}
        }
        if (length>options_.maxFrameBytes) {fail(1009);return result;}
        if (input_.size()-offset<header+4+static_cast<std::size_t>(length)) break;
        std::string payload(static_cast<std::size_t>(length),'\0');
        for (std::size_t n=0;n<payload.size();++n) payload[n]=static_cast<char>(data[header+4+n]^data[header+n%4]);
        offset+=header+4+payload.size();++result.frames;result.consumedBytes=offset;
        if (control) {
            if (opcode==WebSocketOpcode::Close && !closeValid(payload)) {
                fail(payload.size()>=2 && !validUtf8(std::string_view(payload).substr(2))?1007:1002);return result;
            }
            result.events.push_back({opcode,std::move(payload)});continue;
        }
        if ((opcode==WebSocketOpcode::Continuation)==(fragmented_==WebSocketOpcode::Continuation)) {fail(1002);return result;}
        if (opcode!=WebSocketOpcode::Continuation) fragmented_=opcode;
        if (payload.size()>options_.maxMessageBytes-message_.size()) {fail(1009);return result;}
        message_+=payload;
        if (fin) {
            if (fragmented_==WebSocketOpcode::Text && !validUtf8(message_)) {fail(1007);return result;}
            result.events.push_back({fragmented_,std::move(message_)});message_.clear();fragmented_=WebSocketOpcode::Continuation;
        }
    }
    input_.erase(0,offset);
    result.needsContinuation=result.frames==options_.maxFramesPerPush && !input_.empty();
    return result;
}
std::optional<std::string> WebSocketCodec::encodeServer(WebSocketOpcode opcode,std::string_view payload,std::size_t limit) {
    if (payload.size()>limit || payload.size()>(std::numeric_limits<std::size_t>::max)()-10) return std::nullopt;
    if (opcode==WebSocketOpcode::Text) {if (!validUtf8(payload)) return std::nullopt;}
    else if (opcode==WebSocketOpcode::Close) {if (!closeValid(payload)) return std::nullopt;}
    else if (opcode==WebSocketOpcode::Ping || opcode==WebSocketOpcode::Pong) {if (payload.size()>125) return std::nullopt;}
    else if (opcode!=WebSocketOpcode::Binary) return std::nullopt;
    std::string result;result.reserve(payload.size()+10);result+=static_cast<char>(128|static_cast<unsigned>(opcode));
    if (payload.size()<126) result+=static_cast<char>(payload.size());
    else {
        const auto width=payload.size()<=65535?2u:8u;result+=static_cast<char>(width==2?126:127);
        for (unsigned n=width;n;--n) result+=static_cast<char>(std::uint64_t(payload.size())>>((n-1)*8));
    }
    result.append(payload);return result;
}
} // namespace gamenet::websocket
