#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gamenet::protocol {

enum class FrameStatus {
    NeedMoreData,
    FramesReady,
    BudgetExhausted,
    FrameTooLarge,
    BufferLimitExceeded,
    Faulted,
};

struct FrameResult {
    FrameStatus status{FrameStatus::NeedMoreData};
    std::vector<std::string> frames;
    bool needsContinuation{false};
};

struct PacketFramerOptions {
    std::size_t maxPayloadBytes{1024U * 1024U};
    std::size_t maxBufferedBytes{2U * (1024U * 1024U + sizeof(std::uint32_t))};
    std::size_t maxFramesPerPush{64};
    std::size_t maxFrameBytesPerPush{1024U * 1024U + sizeof(std::uint32_t)};
};

class PacketFramer {
public:
    static constexpr std::size_t kLengthBytes = sizeof(std::uint32_t);
    static constexpr std::size_t kDefaultMaxPayloadBytes = 1024U * 1024U;

    explicit PacketFramer(std::size_t maxPayloadBytes = kDefaultMaxPayloadBytes);
    explicit PacketFramer(PacketFramerOptions options);

    FrameResult push(std::string_view bytes);
    std::optional<std::string> encode(std::string_view payload) const;

    void reset() noexcept;
    std::size_t bufferedBytes() const noexcept;
    std::size_t maxPayloadBytes() const noexcept;
    std::size_t maxBufferedBytes() const noexcept;
    std::size_t maxFramesPerPush() const noexcept;
    std::size_t maxFrameBytesPerPush() const noexcept;
    bool faulted() const noexcept;

private:
    char byteAt(std::size_t offset) const noexcept;
    void appendBytes(std::string_view bytes);
    std::string copyPayload(std::size_t offset, std::size_t length) const;
    void consume(std::size_t bytes) noexcept;
    void clearBuffer() noexcept;

    PacketFramerOptions options_;
    std::vector<char> storage_;
    std::size_t head_{};
    std::size_t bufferedBytes_{};
    bool faulted_{false};
};

}  // namespace gamenet::protocol
