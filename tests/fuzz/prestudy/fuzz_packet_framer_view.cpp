// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

// Implements the HP1 prestudy differential contract on one Buffer owner thread.
// Intent: intents/usecases/hp1_packet_framer_view_prestudy.intent.md
// Rules: rules/testing_rules.md and rules/ownership_rules.md section 11.1.1.

#include "hp1/PacketFramerView.h"
#include "gamenet/core/net/Buffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using gamenet::experimental::hp1::FrameVisitStatus;
using gamenet::experimental::hp1::OwnedPacket;
using gamenet::experimental::hp1::PacketFramerViewPrototype;
using gamenet::experimental::hp1::PacketView;
using gamenet::protocol::FrameStatus;
using gamenet::protocol::PacketFramer;
using gamenet::protocol::PacketFramerOptions;

void require(bool condition) {
    if (!condition) std::abort();
}

FrameVisitStatus visitStatus(FrameStatus status) {
    switch (status) {
        case FrameStatus::NeedMoreData: return FrameVisitStatus::NeedMoreData;
        case FrameStatus::FramesReady: return FrameVisitStatus::FramesVisited;
        case FrameStatus::BudgetExhausted: return FrameVisitStatus::BudgetExhausted;
        case FrameStatus::FrameTooLarge: return FrameVisitStatus::FrameTooLarge;
        case FrameStatus::BufferLimitExceeded: return FrameVisitStatus::BufferLimitExceeded;
        case FrameStatus::Faulted: return FrameVisitStatus::Faulted;
    }
    std::abort();
}

class DifferentialFramer {
public:
    explicit DifferentialFramer(PacketFramerOptions options)
        : legacy_(options), candidate_(options) {}

    void pushAndDrain(std::string_view chunk) {
        bool continuation = compareCall(chunk);
        std::size_t rounds = 0;
        while (continuation) {
            require(++rounds <= legacy_.maxBufferedBytes() / PacketFramer::kLengthBytes + 1);
            continuation = compareCall({});
        }
    }

    void reset() {
        legacy_.reset();
        require(candidate_.reset());
        input_.retrieveAll();
        decoded_.clear();
    }

    void verifyRecovery() {
        reset();
        const auto probe = legacy_.encode({});
        require(probe.has_value());
        pushAndDrain(*probe);
        require(decoded_ == std::vector<std::string>{""});
        require(input_.readableBytes() == 0);
    }

    void verifyComplete(const std::vector<std::string>& expected) const {
        require(decoded_ == expected);
        require(input_.readableBytes() == 0);
        require(!legacy_.faulted());
    }

private:
    bool compareCall(std::string_view chunk) {
        const auto expected = legacy_.push(chunk);
        input_.append(chunk);
        const auto readable = std::span(
            reinterpret_cast<const std::byte*>(input_.peek()), input_.readableBytes());
        std::vector<OwnedPacket> retained;
        std::size_t visitedBytes = 0;
        const auto result = candidate_.visitFrames(readable, [&](PacketView view) {
            const auto index = retained.size();
            require(index < expected.frames.size());
            require(view.asStringView() == expected.frames[index]);
            require(view.bytes().data() == readable.data() + visitedBytes + PacketFramer::kLengthBytes);
            retained.push_back(view.retain());
            visitedBytes += PacketFramer::kLengthBytes + view.bytes().size();
        });

        require(result.status == visitStatus(expected.status));
        require(result.frameCount == expected.frames.size());
        require(retained.size() == result.frameCount);
        require(result.consumedBytes == visitedBytes);
        require(result.processedFrameBytes == visitedBytes);
        require(result.frameCount <= legacy_.maxFramesPerPush());
        require(visitedBytes <= legacy_.maxFrameBytesPerPush());
        require(result.needsContinuation == expected.needsContinuation);
        require(candidate_.faulted() == legacy_.faulted());
        require(!result.needsContinuation || visitedBytes != 0);

        // Only the owner mutates Buffer, after every borrowed visitor returned.
        input_.retrieve(result.consumedBytes);
        if (candidate_.faulted()) input_.retrieveAll();
        require(input_.readableBytes() == legacy_.bufferedBytes());
        for (std::size_t index = 0; index < retained.size(); ++index) {
            require(retained[index].asStringView() == expected.frames[index]);
            decoded_.push_back(expected.frames[index]);
        }
        return result.needsContinuation;
    }

    PacketFramer legacy_;
    PacketFramerViewPrototype candidate_;
    gamenet::net::Buffer input_;
    std::vector<std::string> decoded_;
};

void compareArbitraryInput(std::string_view bytes, PacketFramerOptions options) {
    DifferentialFramer whole(options);
    whole.pushAndDrain(bytes);
    whole.pushAndDrain(bytes);  // Includes admission after a sticky protocol fault.
    whole.verifyRecovery();

    DifferentialFramer segmented(options);
    std::size_t calls = 0;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto control = static_cast<unsigned char>(bytes[offset]);
        if (++calls % 8 == 0 && (control & 0x80U) != 0) segmented.reset();
        const auto count = std::min<std::size_t>(1 + control % 97, bytes.size() - offset);
        segmented.pushAndDrain(bytes.substr(offset, count));
        offset += count;
    }
    segmented.pushAndDrain({});
    segmented.verifyRecovery();
}

void compareGeneratedStream(std::string_view bytes, PacketFramerOptions options) {
    PacketFramer encoder(options);
    std::vector<std::string> expected;
    std::string wire;
    for (std::size_t offset = 0; offset < bytes.size() && expected.size() < 32;) {
        const auto selector = static_cast<unsigned char>(bytes[offset++]);
        const auto count = std::min<std::size_t>(
            selector % (options.maxPayloadBytes + 1), bytes.size() - offset);
        const auto payload = bytes.substr(offset, count);
        const auto frame = encoder.encode(payload);
        require(frame.has_value());
        wire.append(*frame);
        expected.emplace_back(payload);
        offset += count;
    }
    if (expected.empty()) {
        wire = *encoder.encode({});
        expected.emplace_back();
    }
    // Valid-stream decoding has enough admission storage in both segmentations.
    options.maxBufferedBytes = std::max(options.maxBufferedBytes, wire.size());
    DifferentialFramer whole(options);
    whole.pushAndDrain(wire);
    whole.verifyComplete(expected);

    DifferentialFramer segmented(options);
    for (std::size_t offset = 0; offset < wire.size();) {
        const auto selector = static_cast<unsigned char>(wire[offset]);
        const auto count = std::min<std::size_t>(1 + selector % 31, wire.size() - offset);
        segmented.pushAndDrain(std::string_view(wire).substr(offset, count));
        offset += count;
    }
    segmented.verifyComplete(expected);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto boundedSize = std::min<std::size_t>(size, 8192);
    const std::string_view bytes(reinterpret_cast<const char*>(data), boundedSize);
    const auto first = boundedSize == 0 ? 0U : data[0];
    const auto last = boundedSize == 0 ? 0U : data[boundedSize - 1];
    constexpr std::array<std::size_t, 5> payloadLimits{64, 0, 1, 4, 256};
    const auto maxPayload = payloadLimits[first % payloadLimits.size()];
    const auto maxFrame = maxPayload + PacketFramer::kLengthBytes;
    const PacketFramerOptions options{
        .maxPayloadBytes = maxPayload,
        .maxBufferedBytes = maxFrame * (1 + last % 4),
        .maxFramesPerPush = 1 + first % 8,
        .maxFrameBytesPerPush = maxFrame * (1 + (last >> 4U) % 4),
    };
    compareArbitraryInput(bytes, options);
    compareGeneratedStream(bytes, options);
    return 0;
}
