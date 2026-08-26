// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerView.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

std::atomic<std::size_t> countedAllocations{};
thread_local bool countAllocations = false;

std::span<const std::byte> readableSpan(const gamenet::net::Buffer& buffer) {
    return {
        reinterpret_cast<const std::byte*>(buffer.peek()),
        buffer.readableBytes(),
    };
}

std::string frame(std::string_view payload) {
    std::string encoded;
    encoded.reserve(sizeof(std::uint32_t) + payload.size());
    const auto size = static_cast<std::uint32_t>(payload.size());
    encoded.push_back(static_cast<char>((size >> 24U) & 0xffU));
    encoded.push_back(static_cast<char>((size >> 16U) & 0xffU));
    encoded.push_back(static_cast<char>((size >> 8U) & 0xffU));
    encoded.push_back(static_cast<char>(size & 0xffU));
    encoded.append(payload);
    return encoded;
}

using gamenet::experimental::hp1::FrameVisitStatus;
using gamenet::experimental::hp1::OwnedPacket;
using gamenet::experimental::hp1::PacketFramerViewPrototype;
using gamenet::experimental::hp1::PacketView;

void testPartialStickyEmptyAndRetain() {
    PacketFramerViewPrototype framer({
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 36,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 36,
    });
    gamenet::net::Buffer input;
    const auto hello = frame("hello");
    input.append(hello.data(), 2);

    std::size_t calls = 0;
    auto result = framer.visitFrames(readableSpan(input), [&](PacketView) { ++calls; });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::NeedMoreData);
    GAMENET_TEST_ASSERT(result.consumedBytes == 0);
    GAMENET_TEST_ASSERT(calls == 0);

    input.append(hello.data() + 2, hello.size() - 2);
    input.append(frame(""));
    input.append(frame("x"));
    std::vector<std::string> observed;
    result = framer.visitFrames(readableSpan(input), [&](PacketView view) {
        observed.emplace_back(view.asStringView());
    });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FramesVisited);
    GAMENET_TEST_ASSERT(result.frameCount == 3);
    GAMENET_TEST_ASSERT(result.consumedBytes == input.readableBytes());
    GAMENET_TEST_ASSERT((observed == std::vector<std::string>{"hello", "", "x"}));
    input.retrieve(result.consumedBytes);
    GAMENET_TEST_ASSERT(input.readableBytes() == 0);

    input.append(frame("retained"));
    std::optional<OwnedPacket> retained;
    result = framer.visitFrames(readableSpan(input), [&](PacketView view) {
        retained.emplace(view.retain());
    });
    GAMENET_TEST_ASSERT(result.frameCount == 1);
    GAMENET_TEST_ASSERT(retained.has_value());
    GAMENET_TEST_ASSERT(retained->asStringView() == "retained");
    input.retrieve(result.consumedBytes);
    GAMENET_TEST_ASSERT(retained->asStringView() == "retained");
}

void testZeroAllocationOwnerLocalVisit() {
    PacketFramerViewPrototype framer({
        .maxPayloadBytes = 32,
        .maxBufferedBytes = 128,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 128,
    });
    gamenet::net::Buffer input;
    input.append(frame("allocation-free"));

    std::size_t checksum = 0;
    countedAllocations.store(0, std::memory_order_relaxed);
    countAllocations = true;
    const auto result = framer.visitFrames(readableSpan(input), [&](PacketView view) noexcept {
        for (const auto value : view.bytes()) {
            checksum += std::to_integer<unsigned char>(value);
        }
    });
    countAllocations = false;

    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FramesVisited);
    GAMENET_TEST_ASSERT(result.frameCount == 1);
    GAMENET_TEST_ASSERT(checksum != 0);
    GAMENET_TEST_ASSERT(countedAllocations.load(std::memory_order_relaxed) == 0);
}

void testBudgetsAndPartialConsumption() {
    PacketFramerViewPrototype framer({
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 48,
        .maxFramesPerPush = 2,
        .maxFrameBytesPerPush = 12,
    });
    gamenet::net::Buffer input;
    const auto first = frame("a");
    const auto second = frame("bb");
    const auto third = frame("ccc");
    const auto partial = frame("tail");
    input.append(first + second + third + partial.substr(0, 5));

    std::vector<std::string> observed;
    auto result = framer.visitFrames(readableSpan(input), [&](PacketView view) {
        observed.emplace_back(view.asStringView());
    });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::BudgetExhausted);
    GAMENET_TEST_ASSERT(result.needsContinuation);
    GAMENET_TEST_ASSERT(result.frameCount == 2);
    GAMENET_TEST_ASSERT(result.consumedBytes == first.size() + second.size());
    input.retrieve(result.consumedBytes);

    result = framer.visitFrames(readableSpan(input), [&](PacketView view) {
        observed.emplace_back(view.asStringView());
    });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FramesVisited);
    GAMENET_TEST_ASSERT(!result.needsContinuation);
    GAMENET_TEST_ASSERT(result.frameCount == 1);
    GAMENET_TEST_ASSERT(result.consumedBytes == third.size());
    input.retrieve(result.consumedBytes);
    GAMENET_TEST_ASSERT(input.readableBytes() == 5);
    GAMENET_TEST_ASSERT((observed == std::vector<std::string>{"a", "bb", "ccc"}));
}

void testFailClosedAndReset() {
    PacketFramerViewPrototype framer({
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 36,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 36,
    });
    gamenet::net::Buffer input;
    const std::string oversizedPrefix{"\x00\x00\x00\x09", 4};
    input.append(frame("valid") + oversizedPrefix);
    std::size_t calls = 0;
    auto result = framer.visitFrames(readableSpan(input), [&](PacketView) { ++calls; });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FrameTooLarge);
    GAMENET_TEST_ASSERT(result.consumedBytes == 0);
    GAMENET_TEST_ASSERT(result.frameCount == 0);
    GAMENET_TEST_ASSERT(calls == 0);
    GAMENET_TEST_ASSERT(framer.faulted());
    GAMENET_TEST_ASSERT(
        framer.visitFrames(readableSpan(input), [&](PacketView) { ++calls; }).status ==
        FrameVisitStatus::Faulted);

    GAMENET_TEST_ASSERT(framer.reset());
    GAMENET_TEST_ASSERT(!framer.faulted());
    input.retrieveAll();
    input.append(frame("ok"));
    result = framer.visitFrames(readableSpan(input), [&](PacketView) { ++calls; });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FramesVisited);
    GAMENET_TEST_ASSERT(calls == 1);

    PacketFramerViewPrototype bounded({
        .maxPayloadBytes = 4,
        .maxBufferedBytes = 8,
        .maxFramesPerPush = 1,
        .maxFrameBytesPerPush = 8,
    });
    input.retrieveAll();
    input.append(frame("a") + frame("b"));
    result = bounded.visitFrames(readableSpan(input), [&](PacketView) { ++calls; });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::BufferLimitExceeded);
    GAMENET_TEST_ASSERT(result.consumedBytes == 0);
    GAMENET_TEST_ASSERT(bounded.faulted());
}

void testVisitorFailureAndReentryRejection() {
    PacketFramerViewPrototype framer({
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 36,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 36,
    });
    gamenet::net::Buffer input;
    const auto first = frame("one");
    input.append(first + frame("two"));
    std::size_t calls = 0;
    auto result = framer.visitFrames(readableSpan(input), [&](PacketView) {
        if (++calls == 2) throw std::runtime_error("contained visitor failure");
    });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::VisitorException);
    GAMENET_TEST_ASSERT(result.frameCount == 1);
    GAMENET_TEST_ASSERT(result.consumedBytes == first.size());
    input.retrieve(result.consumedBytes);
    GAMENET_TEST_ASSERT(input.readableBytes() == frame("two").size());

    PacketFramerViewPrototype reentrant({
        .maxPayloadBytes = 8,
        .maxBufferedBytes = 36,
        .maxFramesPerPush = 8,
        .maxFrameBytesPerPush = 36,
    });
    input.retrieveAll();
    input.append(frame("nested"));
    FrameVisitStatus nestedStatus = FrameVisitStatus::NeedMoreData;
    bool resetAccepted = true;
    result = reentrant.visitFrames(readableSpan(input), [&](PacketView) {
        nestedStatus = reentrant
                           .visitFrames(readableSpan(input), [](PacketView) {})
                           .status;
        resetAccepted = reentrant.reset();
    });
    GAMENET_TEST_ASSERT(result.status == FrameVisitStatus::FramesVisited);
    GAMENET_TEST_ASSERT(nestedStatus == FrameVisitStatus::ReentrantVisitRejected);
    GAMENET_TEST_ASSERT(!resetAccepted);
    GAMENET_TEST_ASSERT(!reentrant.faulted());
}

void testRandomizedLegacyDifferential() {
    const gamenet::protocol::PacketFramerOptions options{
        .maxPayloadBytes = 64,
        .maxBufferedBytes = 512,
        .maxFramesPerPush = 3,
        .maxFrameBytesPerPush = 68 * 3,
    };
    gamenet::protocol::PacketFramer legacy(options);
    PacketFramerViewPrototype candidate(options);
    gamenet::net::Buffer input;
    std::mt19937 random(0x485031U);
    std::uniform_int_distribution<int> payloadSize(0, 64);
    std::uniform_int_distribution<int> byteValue(0, 255);
    std::uniform_int_distribution<int> chunkSize(1, 17);

    std::string wire;
    std::vector<std::string> expected;
    for (std::size_t index = 0; index < 200; ++index) {
        std::string payload(static_cast<std::size_t>(payloadSize(random)), '\0');
        for (auto& value : payload) value = static_cast<char>(byteValue(random));
        wire.append(frame(payload));
        expected.push_back(std::move(payload));
    }

    std::vector<std::string> legacyFrames;
    std::vector<std::string> candidateFrames;
    for (std::size_t offset = 0; offset < wire.size();) {
        const auto count = std::min<std::size_t>(chunkSize(random), wire.size() - offset);
        const std::string_view chunk(wire.data() + offset, count);
        offset += count;
        input.append(chunk);

        auto legacyResult = legacy.push(chunk);
        for (;;) {
            legacyFrames.insert(
                legacyFrames.end(),
                std::make_move_iterator(legacyResult.frames.begin()),
                std::make_move_iterator(legacyResult.frames.end()));
            if (!legacyResult.needsContinuation) break;
            legacyResult = legacy.push({});
        }

        for (;;) {
            const auto candidateResult = candidate.visitFrames(
                readableSpan(input),
                [&](PacketView view) { candidateFrames.emplace_back(view.asStringView()); });
            GAMENET_TEST_ASSERT(
                candidateResult.status == FrameVisitStatus::NeedMoreData ||
                candidateResult.status == FrameVisitStatus::FramesVisited ||
                candidateResult.status == FrameVisitStatus::BudgetExhausted);
            input.retrieve(candidateResult.consumedBytes);
            if (!candidateResult.needsContinuation) break;
        }
    }

    GAMENET_TEST_ASSERT(input.readableBytes() == 0);
    GAMENET_TEST_ASSERT(legacy.bufferedBytes() == 0);
    GAMENET_TEST_ASSERT(legacyFrames == expected);
    GAMENET_TEST_ASSERT(candidateFrames == expected);
}

}  // namespace

void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (countAllocations) countedAllocations.fetch_add(1, std::memory_order_relaxed);
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    static_assert(!std::is_copy_constructible_v<OwnedPacket>);
    static_assert(!std::is_copy_assignable_v<OwnedPacket>);
    static_assert(std::is_move_constructible_v<OwnedPacket>);
    static_assert(std::is_trivially_copyable_v<PacketView>);

    testPartialStickyEmptyAndRetain();
    testZeroAllocationOwnerLocalVisit();
    testBudgetsAndPartialConsumption();
    testFailClosedAndReset();
    testVisitorFailureAndReentryRejection();
    testRandomizedLegacyDifferential();
}
