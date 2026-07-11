#include "gamenet/protocol/PacketFramer.h"

#include <limits>
#include <stdexcept>

namespace gamenet::protocol {
namespace {

std::uint32_t decodeLength(const char* bytes) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

void appendLength(std::string& output, std::uint32_t length) {
    output.push_back(static_cast<char>((length >> 24U) & 0xffU));
    output.push_back(static_cast<char>((length >> 16U) & 0xffU));
    output.push_back(static_cast<char>((length >> 8U) & 0xffU));
    output.push_back(static_cast<char>(length & 0xffU));
}

}  // namespace

PacketFramer::PacketFramer(std::size_t maxPayloadBytes)
    : maxPayloadBytes_(maxPayloadBytes) {
    if (maxPayloadBytes_ > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("PacketFramer maximum exceeds uint32 wire length");
    }
}

FrameResult PacketFramer::push(std::string_view bytes) {
    if (faulted_) {
        return {.status = FrameStatus::Faulted, .frames = {}};
    }

    if (!bytes.empty()) {
        buffer_.append(bytes.data(), bytes.size());
    }
    FrameResult result;
    std::size_t consumed = 0;

    while (buffer_.size() - consumed >= kLengthBytes) {
        const auto payloadBytes = static_cast<std::size_t>(decodeLength(buffer_.data() + consumed));
        if (payloadBytes > maxPayloadBytes_) {
            buffer_.clear();
            faulted_ = true;
            return {.status = FrameStatus::FrameTooLarge, .frames = {}};
        }

        const auto frameBytes = kLengthBytes + payloadBytes;
        if (buffer_.size() - consumed < frameBytes) {
            break;
        }

        result.frames.emplace_back(buffer_.data() + consumed + kLengthBytes, payloadBytes);
        consumed += frameBytes;
    }

    if (consumed != 0) {
        buffer_.erase(0, consumed);
    }
    result.status = result.frames.empty() ? FrameStatus::NeedMoreData : FrameStatus::FramesReady;
    return result;
}

std::optional<std::string> PacketFramer::encode(std::string_view payload) const {
    if (payload.size() > maxPayloadBytes_ ||
        payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }

    std::string output;
    output.reserve(kLengthBytes + payload.size());
    appendLength(output, static_cast<std::uint32_t>(payload.size()));
    output.append(payload.data(), payload.size());
    return output;
}

void PacketFramer::reset() noexcept {
    buffer_.clear();
    faulted_ = false;
}

std::size_t PacketFramer::bufferedBytes() const noexcept {
    return buffer_.size();
}

std::size_t PacketFramer::maxPayloadBytes() const noexcept {
    return maxPayloadBytes_;
}

bool PacketFramer::faulted() const noexcept {
    return faulted_;
}

}  // namespace gamenet::protocol
