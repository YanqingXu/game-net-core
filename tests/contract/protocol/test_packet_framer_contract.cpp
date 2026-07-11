#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <string>

using gamenet::protocol::FrameStatus;
using gamenet::protocol::PacketFramer;

int main() {
    PacketFramer framer(8);

    auto encoded = framer.encode("hello");
    GAMENET_TEST_ASSERT(encoded.has_value());
    GAMENET_TEST_ASSERT(encoded->size() == 9);
    GAMENET_TEST_ASSERT(static_cast<unsigned char>((*encoded)[0]) == 0);
    GAMENET_TEST_ASSERT(static_cast<unsigned char>((*encoded)[3]) == 5);

    auto result = framer.push(std::string_view(*encoded).substr(0, 2));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::NeedMoreData);
    GAMENET_TEST_ASSERT(framer.bufferedBytes() == 2);
    result = framer.push(std::string_view(*encoded).substr(2, 3));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::NeedMoreData);
    result = framer.push(std::string_view(*encoded).substr(5));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1);
    GAMENET_TEST_ASSERT(result.frames[0] == "hello");
    GAMENET_TEST_ASSERT(framer.bufferedBytes() == 0);

    auto empty = framer.encode("");
    auto shortFrame = framer.encode("x");
    GAMENET_TEST_ASSERT(empty.has_value() && shortFrame.has_value());
    result = framer.push(*empty + *shortFrame);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 2);
    GAMENET_TEST_ASSERT(result.frames[0].empty());
    GAMENET_TEST_ASSERT(result.frames[1] == "x");

    const std::string malicious{"\x00\x00\x00\x09", 4};
    result = framer.push(malicious);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FrameTooLarge);
    GAMENET_TEST_ASSERT(result.frames.empty());
    GAMENET_TEST_ASSERT(framer.faulted());
    GAMENET_TEST_ASSERT(framer.push(*shortFrame).status == FrameStatus::Faulted);
    GAMENET_TEST_ASSERT(!framer.encode("123456789").has_value());

    framer.reset();
    GAMENET_TEST_ASSERT(!framer.faulted());
    result = framer.push(*shortFrame);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames[0] == "x");
}
