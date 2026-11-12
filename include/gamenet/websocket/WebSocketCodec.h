// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gamenet::websocket {
enum class WebSocketOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };
struct WebSocketRequest { std::string target, host, origin; };
struct WebSocketUpgrade {
    unsigned httpStatus{}; // 0 = incomplete; 101 = accepted; otherwise bounded HTTP rejection
    std::size_t consumedBytes{};
    WebSocketRequest request;
    std::string response;
};
// Strict server-side HTTP Upgrade only; no body, extensions or subprotocol selection.
WebSocketUpgrade parseWebSocketUpgrade(std::string_view input,
    std::size_t maxHeaderBytes = 8192, std::size_t maxHeaderFields = 32);
struct WebSocketCodecOptions {
    std::size_t maxFrameBytes{64 * 1024}, maxMessageBytes{256 * 1024};
    std::size_t maxBufferedBytes{512 * 1024}, maxFramesPerPush{32};
};
struct WebSocketEvent { WebSocketOpcode opcode; std::string payload; };
struct WebSocketDecodeResult {
    std::vector<WebSocketEvent> events;
    std::size_t frames{}, consumedBytes{};
    bool needsContinuation{};
    std::uint16_t errorCode{}; // sticky 1002 / 1007 / 1009; valid prefix events remain ordered
};
// Independently owned codec state; receives masked client frames only.
class WebSocketCodec {
public:
    explicit WebSocketCodec(WebSocketCodecOptions options = {});
    WebSocketDecodeResult push(std::string_view bytes);
    void reset() noexcept;
    std::size_t bufferedBytes() const noexcept;
    std::size_t fragmentedBytes() const noexcept;
    static bool validUtf8(std::string_view bytes) noexcept;
    static std::optional<std::string> encodeServer(WebSocketOpcode opcode,
        std::string_view payload, std::size_t maxFrameBytes = 64 * 1024);
private:
    WebSocketCodecOptions options_;
    std::string input_, message_;
    WebSocketOpcode fragmented_{WebSocketOpcode::Continuation};
    std::uint16_t fault_{};
};
} // namespace gamenet::websocket
