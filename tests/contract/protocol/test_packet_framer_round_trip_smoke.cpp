#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

int main() {
    std::mt19937 generator(0x474e4554U);
    std::uniform_int_distribution<int> lengthDistribution(0, 128);
    std::uniform_int_distribution<int> byteDistribution(0, 255);
    std::uniform_int_distribution<int> chunkDistribution(1, 11);

    std::vector<std::string> expected;
    std::string stream;
    gamenet::protocol::PacketFramer encoder(128);
    for (int iteration = 0; iteration < 500; ++iteration) {
        std::string payload(static_cast<std::size_t>(lengthDistribution(generator)), '\0');
        for (char& value : payload) {
            value = static_cast<char>(byteDistribution(generator));
        }
        expected.push_back(payload);
        auto frame = encoder.encode(payload);
        GAMENET_TEST_ASSERT(frame.has_value());
        stream += *frame;
    }

    gamenet::protocol::PacketFramer decoder(128);
    std::vector<std::string> actual;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto chunk = std::min<std::size_t>(
            static_cast<std::size_t>(chunkDistribution(generator)), stream.size() - offset);
        auto result = decoder.push(std::string_view(stream).substr(offset, chunk));
        GAMENET_TEST_ASSERT(result.status != gamenet::protocol::FrameStatus::FrameTooLarge);
        GAMENET_TEST_ASSERT(result.status != gamenet::protocol::FrameStatus::BufferLimitExceeded);
        GAMENET_TEST_ASSERT(result.status != gamenet::protocol::FrameStatus::Faulted);
        actual.insert(actual.end(), result.frames.begin(), result.frames.end());
        while (result.needsContinuation) {
            result = decoder.push({});
            actual.insert(actual.end(), result.frames.begin(), result.frames.end());
        }
        offset += chunk;
    }

    GAMENET_TEST_ASSERT(actual == expected);
    GAMENET_TEST_ASSERT(decoder.bufferedBytes() == 0);
}
