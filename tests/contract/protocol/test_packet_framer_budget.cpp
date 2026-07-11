#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using gamenet::protocol::FrameStatus;
    using gamenet::protocol::PacketFramer;
    using gamenet::protocol::PacketFramerOptions;

    PacketFramer bounded(PacketFramerOptions{
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 36,
        .maxFramesPerPush = 2,
        .maxFrameBytesPerPush = 24,
    });
    auto first = bounded.encode("a");
    auto second = bounded.encode("bb");
    auto third = bounded.encode("ccc");
    GAMENET_TEST_ASSERT(first && second && third);

    auto result = bounded.push(*first + *second + *third);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BudgetExhausted);
    GAMENET_TEST_ASSERT(result.needsContinuation);
    GAMENET_TEST_ASSERT(result.frames.size() == 2);
    GAMENET_TEST_ASSERT(result.frames[0] == "a");
    GAMENET_TEST_ASSERT(result.frames[1] == "bb");
    GAMENET_TEST_ASSERT(bounded.bufferedBytes() == third->size());

    result = bounded.push({});
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(!result.needsContinuation);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "ccc");
    GAMENET_TEST_ASSERT(bounded.bufferedBytes() == 0);

    PacketFramer byteBounded(PacketFramerOptions{
        .maxPayloadBytes = 4,
        .maxBufferedBytes = 24,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 8,
    });
    auto small = byteBounded.encode("x");
    GAMENET_TEST_ASSERT(small);
    result = byteBounded.push(*small + *small);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BudgetExhausted);
    GAMENET_TEST_ASSERT(result.frames.size() == 1);
    result = byteBounded.push({});
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1);

    PacketFramer emptyFlood(PacketFramerOptions{
        .maxPayloadBytes = 1,
        .maxBufferedBytes = 12,
        .maxFramesPerPush = 2,
        .maxFrameBytesPerPush = 8,
    });
    auto empty = emptyFlood.encode("");
    GAMENET_TEST_ASSERT(empty);
    result = emptyFlood.push(*empty + *empty + *empty);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BudgetExhausted);
    GAMENET_TEST_ASSERT(result.frames.size() == 2);
    result = emptyFlood.push({});
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0].empty());

    PacketFramer exactByteBudget(PacketFramerOptions{
        .maxPayloadBytes = 4,
        .maxBufferedBytes = 24,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 8,
    });
    const auto exactEmpty = exactByteBudget.encode("");
    const auto oneByte = exactByteBudget.encode("x");
    GAMENET_TEST_ASSERT(exactEmpty && oneByte);
    result = exactByteBudget.push(*exactEmpty + *exactEmpty);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 2);
    result = exactByteBudget.push(*exactEmpty + *oneByte);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BudgetExhausted);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0].empty());
    result = exactByteBudget.push({});
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "x");

    constexpr std::size_t stickyFrameCount = 128;
    PacketFramer stickyFlood(PacketFramerOptions{
        .maxPayloadBytes = 1,
        .maxBufferedBytes = stickyFrameCount * 5,
        .maxFramesPerPush = 7,
        .maxFrameBytesPerPush = 35,
    });
    std::string stickyBytes;
    std::vector<std::string> stickyExpected;
    for (std::size_t index = 0; index < stickyFrameCount; ++index) {
        stickyExpected.emplace_back(1, static_cast<char>('a' + (index % 26)));
        const auto frame = stickyFlood.encode(stickyExpected.back());
        GAMENET_TEST_ASSERT(frame);
        stickyBytes += *frame;
    }
    std::vector<std::string> stickyActual;
    result = stickyFlood.push(stickyBytes);
    for (;;) {
        stickyActual.insert(stickyActual.end(), result.frames.begin(), result.frames.end());
        if (!result.needsContinuation) break;
        GAMENET_TEST_ASSERT(result.status == FrameStatus::BudgetExhausted);
        result = stickyFlood.push({});
    }
    GAMENET_TEST_ASSERT(stickyActual == stickyExpected);
    GAMENET_TEST_ASSERT(stickyFlood.bufferedBytes() == 0);

    PacketFramer wrapped(PacketFramerOptions{
        .maxPayloadBytes = 5,
        .maxBufferedBytes = 24,
        .maxFramesPerPush = 1,
        .maxFrameBytesPerPush = 9,
    });
    const auto wrapA = wrapped.encode("a");
    const auto wrapB = wrapped.encode("bb");
    const auto wrapC = wrapped.encode("ccc");
    const auto wrapD = wrapped.encode("ddddd");
    GAMENET_TEST_ASSERT(wrapA && wrapB && wrapC && wrapD);
    result = wrapped.push(*wrapA + *wrapB + *wrapC);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "a");
    result = wrapped.push(*wrapD);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "bb");
    result = wrapped.push({});
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "ccc");
    result = wrapped.push({});
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "ddddd");
    GAMENET_TEST_ASSERT(wrapped.bufferedBytes() == 0);

    PacketFramer overflow(PacketFramerOptions{
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 12,
        .maxFramesPerPush = 2,
        .maxFrameBytesPerPush = 12,
    });
    result = overflow.push(std::string(13, 'x'));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BufferLimitExceeded);
    GAMENET_TEST_ASSERT(result.frames.empty());
    GAMENET_TEST_ASSERT(overflow.faulted());
    GAMENET_TEST_ASSERT(overflow.bufferedBytes() == 0);
    GAMENET_TEST_ASSERT(overflow.push("ignored").status == FrameStatus::Faulted);
    overflow.reset();
    GAMENET_TEST_ASSERT(!overflow.faulted());
    const auto recovered = overflow.encode("12345678");
    GAMENET_TEST_ASSERT(recovered && recovered->size() == 12);
    result = overflow.push(std::string_view(*recovered).substr(0, recovered->size() - 1));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::NeedMoreData);
    GAMENET_TEST_ASSERT(result.frames.empty());
    GAMENET_TEST_ASSERT(overflow.bufferedBytes() == recovered->size() - 1);
    result = overflow.push(std::string_view(*recovered).substr(recovered->size() - 1));
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1 && result.frames[0] == "12345678");
    GAMENET_TEST_ASSERT(overflow.bufferedBytes() == 0);

    PacketFramer exactBuffer(PacketFramerOptions{
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 12,
        .maxFramesPerPush = 1,
        .maxFrameBytesPerPush = 12,
    });
    result = exactBuffer.push(*recovered);
    GAMENET_TEST_ASSERT(result.status == FrameStatus::FramesReady);
    GAMENET_TEST_ASSERT(result.frames.size() == 1);
    PacketFramer onePastBuffer(PacketFramerOptions{
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 12,
        .maxFramesPerPush = 1,
        .maxFrameBytesPerPush = 12,
    });
    result = onePastBuffer.push(*recovered + "x");
    GAMENET_TEST_ASSERT(result.status == FrameStatus::BufferLimitExceeded);
    GAMENET_TEST_ASSERT(onePastBuffer.faulted());

    bool rejectedInvalidOptions = false;
    try {
        PacketFramer invalid(PacketFramerOptions{
            .maxPayloadBytes = 8,
            .maxBufferedBytes = 11,
            .maxFramesPerPush = 1,
            .maxFrameBytesPerPush = 12,
        });
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejectedInvalidOptions = true;
    }
    GAMENET_TEST_ASSERT(rejectedInvalidOptions);
}
