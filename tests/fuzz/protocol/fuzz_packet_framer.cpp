#include "gamenet/protocol/PacketFramer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using gamenet::protocol::FrameResult;
using gamenet::protocol::PacketFramer;

void require(bool condition) {
    if (!condition) std::abort();
}

void collect(PacketFramer& framer, FrameResult result, std::vector<std::string>& frames) {
    frames.insert(
        frames.end(),
        std::make_move_iterator(result.frames.begin()),
        std::make_move_iterator(result.frames.end()));
    std::size_t continuations = 0;
    while (result.needsContinuation && !framer.faulted()) {
        require(++continuations <= framer.maxBufferedBytes() / PacketFramer::kLengthBytes + 1);
        result = framer.push({});
        frames.insert(
            frames.end(),
            std::make_move_iterator(result.frames.begin()),
            std::make_move_iterator(result.frames.end()));
    }
    require(framer.bufferedBytes() <= framer.maxBufferedBytes());
}

std::vector<std::string> decodeValidStream(std::string_view stream, std::size_t stride) {
    PacketFramer framer({
        .maxPayloadBytes = 64,
        .maxBufferedBytes = 16 * 1024,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 68,
    });
    std::vector<std::string> frames;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto chunk = std::min(stride, stream.size() - offset);
        collect(framer, framer.push(stream.substr(offset, chunk)), frames);
        offset += chunk;
    }
    collect(framer, framer.push({}), frames);
    require(!framer.faulted());
    require(framer.bufferedBytes() == 0);
    return frames;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    PacketFramer framer(
        gamenet::protocol::PacketFramerOptions{
            .maxPayloadBytes = 4096,
            .maxBufferedBytes = 8192,
            .maxFramesPerPush = 8,
            .maxFrameBytesPerPush = 4100,
        });

    const auto bytes = std::string_view(reinterpret_cast<const char*>(data), size);
    std::size_t offset = 0;
    const std::size_t stride = size == 0 ? 1 : 1 + (data[0] % 31U);
    std::vector<std::string> arbitraryFrames;
    while (offset < bytes.size() && !framer.faulted()) {
        const auto chunk = std::min(stride, bytes.size() - offset);
        collect(framer, framer.push(bytes.substr(offset, chunk)), arbitraryFrames);
        offset += chunk;
    }
    if (!framer.faulted()) collect(framer, framer.push({}), arbitraryFrames);
    for (const auto& frame : arbitraryFrames) require(frame.size() <= framer.maxPayloadBytes());

    framer.reset();
    const auto resetFrame = framer.encode("reset-ok");
    require(resetFrame.has_value());
    std::vector<std::string> resetFrames;
    collect(framer, framer.push(*resetFrame), resetFrames);
    require(resetFrames == std::vector<std::string>{"reset-ok"});

    PacketFramer encoder(64);
    std::vector<std::string> expected;
    std::string validStream;
    for (std::size_t offset = 0; offset < size && expected.size() < 32;) {
        const auto payloadBytes = std::min<std::size_t>(data[offset] % 65U, size - offset - 1);
        std::string payload(
            reinterpret_cast<const char*>(data + offset + 1), payloadBytes);
        expected.push_back(payload);
        const auto encoded = encoder.encode(payload);
        require(encoded.has_value());
        validStream += *encoded;
        offset += payloadBytes + 1;
    }
    if (!expected.empty()) {
        const auto oneShot = decodeValidStream(validStream, std::max<std::size_t>(validStream.size(), 1));
        const auto chunked = decodeValidStream(validStream, stride);
        require(oneShot == expected);
        require(chunked == expected);
        require(oneShot == chunked);
    }
    return 0;
}
