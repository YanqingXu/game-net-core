#include "gamenet/protocol/PacketFramer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace gamenet::protocol {
namespace {

void appendLength(std::string& output, std::uint32_t length) {
    output.push_back(static_cast<char>((length >> 24U) & 0xffU));
    output.push_back(static_cast<char>((length >> 16U) & 0xffU));
    output.push_back(static_cast<char>((length >> 8U) & 0xffU));
    output.push_back(static_cast<char>(length & 0xffU));
}

PacketFramerOptions optionsForPayloadLimit(std::size_t maxPayloadBytes) {
    if (maxPayloadBytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        maxPayloadBytes > std::numeric_limits<std::size_t>::max() - PacketFramer::kLengthBytes) {
        throw std::invalid_argument("PacketFramer maximum exceeds uint32 wire length");
    }
    const auto frameBytes = PacketFramer::kLengthBytes + maxPayloadBytes;
    const auto maxBufferedBytes =
        frameBytes <= std::numeric_limits<std::size_t>::max() / 2U ? frameBytes * 2U : frameBytes;
    return {
        .maxPayloadBytes = maxPayloadBytes,
        .maxBufferedBytes = maxBufferedBytes,
        .maxFramesPerPush = 64,
        .maxFrameBytesPerPush = frameBytes,
    };
}

}  // namespace

PacketFramer::PacketFramer(std::size_t maxPayloadBytes)
    : PacketFramer(optionsForPayloadLimit(maxPayloadBytes)) {}

PacketFramer::PacketFramer(PacketFramerOptions options) : options_(options) {
    if (options_.maxPayloadBytes >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        options_.maxPayloadBytes >
            std::numeric_limits<std::size_t>::max() - kLengthBytes) {
        throw std::invalid_argument("PacketFramer maximum exceeds uint32 wire length");
    }
    const auto maximumFrameBytes = kLengthBytes + options_.maxPayloadBytes;
    if (options_.maxBufferedBytes < maximumFrameBytes || options_.maxFramesPerPush == 0 ||
        options_.maxFrameBytesPerPush < maximumFrameBytes) {
        throw std::invalid_argument("PacketFramer requires coherent non-zero processing limits");
    }
}

FrameResult PacketFramer::push(std::string_view bytes) {
    if (faulted_) {
        return {.status = FrameStatus::Faulted, .frames = {}};
    }

    if (bytes.size() > options_.maxBufferedBytes - bufferedBytes_) {
        clearBuffer();
        faulted_ = true;
        return {.status = FrameStatus::BufferLimitExceeded, .frames = {}, .needsContinuation = false};
    }
    if (!bytes.empty()) {
        appendBytes(bytes);
    }
    FrameResult result;
    std::size_t consumed = 0;
    std::size_t processedFrameBytes = 0;
    bool budgetExhausted = false;

    while (bufferedBytes_ - consumed >= kLengthBytes) {
        const auto payloadBytes =
            (static_cast<std::size_t>(static_cast<unsigned char>(byteAt(consumed))) << 24U) |
            (static_cast<std::size_t>(static_cast<unsigned char>(byteAt(consumed + 1))) << 16U) |
            (static_cast<std::size_t>(static_cast<unsigned char>(byteAt(consumed + 2))) << 8U) |
            static_cast<std::size_t>(static_cast<unsigned char>(byteAt(consumed + 3)));
        if (payloadBytes > options_.maxPayloadBytes) {
            clearBuffer();
            faulted_ = true;
            return {.status = FrameStatus::FrameTooLarge, .frames = {}, .needsContinuation = false};
        }

        const auto frameBytes = kLengthBytes + payloadBytes;
        if (bufferedBytes_ - consumed < frameBytes) {
            break;
        }
        if (result.frames.size() >= options_.maxFramesPerPush ||
            frameBytes > options_.maxFrameBytesPerPush - processedFrameBytes) {
            budgetExhausted = true;
            break;
        }

        result.frames.push_back(copyPayload(consumed + kLengthBytes, payloadBytes));
        consumed += frameBytes;
        processedFrameBytes += frameBytes;
    }

    if (consumed != 0) {
        consume(consumed);
    }
    result.needsContinuation = budgetExhausted;
    if (budgetExhausted) {
        result.status = FrameStatus::BudgetExhausted;
    } else {
        result.status = result.frames.empty() ? FrameStatus::NeedMoreData : FrameStatus::FramesReady;
    }
    return result;
}

std::optional<std::string> PacketFramer::encode(std::string_view payload) const {
    if (payload.size() > options_.maxPayloadBytes ||
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
    clearBuffer();
    faulted_ = false;
}

std::size_t PacketFramer::bufferedBytes() const noexcept {
    return bufferedBytes_;
}

std::size_t PacketFramer::maxPayloadBytes() const noexcept {
    return options_.maxPayloadBytes;
}

std::size_t PacketFramer::maxBufferedBytes() const noexcept { return options_.maxBufferedBytes; }

std::size_t PacketFramer::maxFramesPerPush() const noexcept {
    return options_.maxFramesPerPush;
}

std::size_t PacketFramer::maxFrameBytesPerPush() const noexcept {
    return options_.maxFrameBytesPerPush;
}

bool PacketFramer::faulted() const noexcept {
    return faulted_;
}

char PacketFramer::byteAt(std::size_t offset) const noexcept {
    return storage_[(head_ + offset) % storage_.size()];
}

void PacketFramer::appendBytes(std::string_view bytes) {
    const auto required = bufferedBytes_ + bytes.size();
    if (storage_.size() < required) {
        std::size_t grownCapacity = storage_.empty() ? std::min<std::size_t>(64, options_.maxBufferedBytes)
                                                     : storage_.size();
        while (grownCapacity < required) {
            const auto availableGrowth = options_.maxBufferedBytes - grownCapacity;
            const auto growth = std::min(
                availableGrowth, std::max<std::size_t>(grownCapacity / 2, 1));
            grownCapacity += growth;
            if (grownCapacity < required && grownCapacity == options_.maxBufferedBytes) {
                grownCapacity = required;
            }
        }

        std::vector<char> grown(grownCapacity);
        if (bufferedBytes_ != 0) {
            const auto first = std::min(bufferedBytes_, storage_.size() - head_);
            std::memcpy(grown.data(), storage_.data() + head_, first);
            if (first != bufferedBytes_) {
                std::memcpy(grown.data() + first, storage_.data(), bufferedBytes_ - first);
            }
        }
        storage_.swap(grown);
        head_ = 0;
    }

    const auto tail = (head_ + bufferedBytes_) % storage_.size();
    const auto first = std::min(bytes.size(), storage_.size() - tail);
    std::memcpy(storage_.data() + tail, bytes.data(), first);
    if (first != bytes.size()) {
        std::memcpy(storage_.data(), bytes.data() + first, bytes.size() - first);
    }
    bufferedBytes_ += bytes.size();
}

std::string PacketFramer::copyPayload(std::size_t offset, std::size_t length) const {
    std::string payload(length, '\0');
    if (length == 0) return payload;
    const auto begin = (head_ + offset) % storage_.size();
    const auto first = std::min(length, storage_.size() - begin);
    std::memcpy(payload.data(), storage_.data() + begin, first);
    if (first != length) {
        std::memcpy(payload.data() + first, storage_.data(), length - first);
    }
    return payload;
}

void PacketFramer::consume(std::size_t bytes) noexcept {
    head_ = (head_ + bytes) % storage_.size();
    bufferedBytes_ -= bytes;
    if (bufferedBytes_ == 0) head_ = 0;
}

void PacketFramer::clearBuffer() noexcept {
    storage_.clear();
    head_ = 0;
    bufferedBytes_ = 0;
}

}  // namespace gamenet::protocol
