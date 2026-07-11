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
    FrameTooLarge,
    Faulted,
};

struct FrameResult {
    FrameStatus status{FrameStatus::NeedMoreData};
    std::vector<std::string> frames;
};

class PacketFramer {
public:
    static constexpr std::size_t kLengthBytes = sizeof(std::uint32_t);
    static constexpr std::size_t kDefaultMaxPayloadBytes = 1024U * 1024U;

    explicit PacketFramer(std::size_t maxPayloadBytes = kDefaultMaxPayloadBytes);

    FrameResult push(std::string_view bytes);
    std::optional<std::string> encode(std::string_view payload) const;

    void reset() noexcept;
    std::size_t bufferedBytes() const noexcept;
    std::size_t maxPayloadBytes() const noexcept;
    bool faulted() const noexcept;

private:
    std::size_t maxPayloadBytes_;
    std::string buffer_;
    bool faulted_{false};
};

}  // namespace gamenet::protocol
