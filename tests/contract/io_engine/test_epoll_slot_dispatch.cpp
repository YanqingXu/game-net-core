// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp3/EpollSlotDispatch.h"

#include "support/TestAssert.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {

using namespace gamenet::experimental::hp3;

struct Target {
    int calls{};
};

void testCapacityIdentityUpdateAndExactCancel() {
    EpollSlotDispatchPrototype arena({.capacity = 2, .maxNoticesPerBatch = 2});
    Target first;
    Target second;
    Target conflicting;

    const auto one = arena.registerOrUpdate(10, &first, kReadEvent);
    const auto two = arena.registerOrUpdate(11, &second, kWriteEvent);
    GAMENET_TEST_ASSERT(one.result == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(two.result == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(one.identity.valid());
    GAMENET_TEST_ASSERT(one.identity.token() != kWakeupToken);
    GAMENET_TEST_ASSERT(arena.isCurrent(one.identity, &first));
    GAMENET_TEST_ASSERT(
        arena.registerOrUpdate(12, &conflicting, kReadEvent).result ==
        SlotDispatchResult::Capacity);
    GAMENET_TEST_ASSERT(
        arena.registerOrUpdate(10, &conflicting, kReadEvent).result ==
        SlotDispatchResult::Conflict);

    const auto disabled = arena.registerOrUpdate(10, &first, 0);
    const auto reenabled = arena.registerOrUpdate(10, &first, kWriteEvent);
    GAMENET_TEST_ASSERT(disabled.identity == one.identity);
    GAMENET_TEST_ASSERT(reenabled.identity == one.identity);
    GAMENET_TEST_ASSERT(
        arena.cancel(one.identity, &conflicting) == SlotDispatchResult::Conflict);
    GAMENET_TEST_ASSERT(
        arena.cancel(one.identity, &first) == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(!arena.isCurrent(one.identity, &first));

    const auto replacement = arena.registerOrUpdate(10, &conflicting, kReadEvent);
    GAMENET_TEST_ASSERT(replacement.result == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(replacement.identity.slotIndex == one.identity.slotIndex);
    GAMENET_TEST_ASSERT(replacement.identity.generation != one.identity.generation);
    GAMENET_TEST_ASSERT(
        arena.cancel(one.identity, &first) == SlotDispatchResult::Conflict);

    GAMENET_TEST_ASSERT(
        arena.cancel(two.identity, &second) == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(
        arena.cancel(replacement.identity, &conflicting) == SlotDispatchResult::Accepted);
    arena.beginStop();
    GAMENET_TEST_ASSERT(arena.tryShutdown());
    GAMENET_TEST_ASSERT(arena.snapshot().settled());
}

void testO1DecodeMergeFilteringAndStaleTokens() {
    EpollSlotDispatchPrototype arena({.capacity = 4, .maxNoticesPerBatch = 4});
    Target first;
    Target second;
    const auto one = arena.registerOrUpdate(21, &first, kReadEvent).identity;
    const auto two = arena.registerOrUpdate(22, &second, kWriteEvent).identity;

    const std::vector<NativeReadinessEvent> native{
        {.token = kWakeupToken, .events = kReadEvent},
        {.token = one.token(), .events = kReadEvent},
        {.token = one.token(), .events = kErrorEvent},
        {.token = two.token(), .events = kReadEvent},
        {.token = two.token(), .events = kWriteEvent | kCloseEvent},
        {.token = (static_cast<std::uint64_t>(1) << 32U), .events = kReadEvent},
        {.token = (static_cast<std::uint64_t>(99) << 32U) | 99U,
         .events = kReadEvent},
    };
    const auto batch = arena.decode(native);
    GAMENET_TEST_ASSERT(batch.notices.size() == 2);
    GAMENET_TEST_ASSERT(batch.notices[0].identity == one);
    GAMENET_TEST_ASSERT(batch.notices[0].events == (kReadEvent | kErrorEvent));
    GAMENET_TEST_ASSERT(batch.notices[1].identity == two);
    GAMENET_TEST_ASSERT(batch.notices[1].events == (kWriteEvent | kCloseEvent));
    GAMENET_TEST_ASSERT(batch.metrics.nativeEvents == native.size());
    GAMENET_TEST_ASSERT(batch.metrics.slotProbes == 4);
    GAMENET_TEST_ASSERT(batch.metrics.waitHashLookups == 0);
    GAMENET_TEST_ASSERT(batch.metrics.mergeProbes == 0);
    GAMENET_TEST_ASSERT(batch.metrics.mergedEvents == 1);
    GAMENET_TEST_ASSERT(batch.metrics.wakeupEvents == 1);
    GAMENET_TEST_ASSERT(batch.metrics.filteredEvents == 1);
    GAMENET_TEST_ASSERT(batch.metrics.malformedEvents == 2);

    arena.finishBatch();
    GAMENET_TEST_ASSERT(arena.cancel(one, &first) == SlotDispatchResult::Accepted);
    const auto stale = arena.decode({&native[1], 1});
    GAMENET_TEST_ASSERT(stale.notices.empty());
    GAMENET_TEST_ASSERT(stale.metrics.staleEvents == 1);
    arena.finishBatch();
    GAMENET_TEST_ASSERT(arena.cancel(two, &second) == SlotDispatchResult::Accepted);
    arena.beginStop();
    GAMENET_TEST_ASSERT(arena.tryShutdown());
}

void testBatchBudgetEpochWrapAndActiveBatchInvalidation() {
    EpollSlotDispatchPrototype arena({.capacity = 3, .maxNoticesPerBatch = 2});
    Target first;
    Target second;
    Target third;
    Target replacement;
    const auto one = arena.registerOrUpdate(31, &first, kReadEvent).identity;
    const auto two = arena.registerOrUpdate(32, &second, kReadEvent).identity;
    const auto three = arena.registerOrUpdate(33, &third, kReadEvent).identity;

    const std::vector<NativeReadinessEvent> native{
        {.token = one.token(), .events = kReadEvent},
        {.token = two.token(), .events = kReadEvent},
        {.token = three.token(), .events = kReadEvent},
    };
    auto batch = arena.decode(native);
    GAMENET_TEST_ASSERT(batch.notices.size() == 2);
    GAMENET_TEST_ASSERT(batch.metrics.budgetExhausted);
    GAMENET_TEST_ASSERT(batch.metrics.budgetDroppedEvents == 1);

    // Simulate callback one invalidating an undispatched second notice.
    const auto staleSecond = batch.notices[1];
    ++first.calls;
    GAMENET_TEST_ASSERT(arena.cancel(two, &second) == SlotDispatchResult::Accepted);
    const auto replacementIdentity =
        arena.registerOrUpdate(32, &replacement, kReadEvent).identity;
    GAMENET_TEST_ASSERT(!arena.isCurrent(staleSecond.identity, staleSecond.target));
    GAMENET_TEST_ASSERT(replacementIdentity.generation != staleSecond.identity.generation);
    GAMENET_TEST_ASSERT(second.calls == 0);
    GAMENET_TEST_ASSERT(replacement.calls == 0);

    arena.finishBatch();
    arena.forceBatchEpochForTesting((std::numeric_limits<std::uint32_t>::max)());
    const NativeReadinessEvent duplicate[] = {
        {.token = one.token(), .events = kReadEvent},
        {.token = one.token(), .events = kErrorEvent},
    };
    batch = arena.decode(duplicate);
    GAMENET_TEST_ASSERT(batch.metrics.batchEpoch == 1);
    GAMENET_TEST_ASSERT(batch.metrics.epochResets == 1);
    GAMENET_TEST_ASSERT(batch.notices.size() == 1);
    GAMENET_TEST_ASSERT(batch.metrics.mergedEvents == 1);
    arena.finishBatch();

    GAMENET_TEST_ASSERT(arena.cancel(one, &first) == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(arena.cancel(three, &third) == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(
        arena.cancel(replacementIdentity, &replacement) == SlotDispatchResult::Accepted);
    arena.beginStop();
    GAMENET_TEST_ASSERT(arena.tryShutdown());
}

void testOwnerStopAndFailurePaths() {
    bool invalidOptionsRejected = false;
    try {
        EpollSlotDispatchPrototype invalid({.capacity = 1, .maxNoticesPerBatch = 2});
    } catch (const std::invalid_argument&) {
        invalidOptionsRejected = true;
    }
    GAMENET_TEST_ASSERT(invalidOptionsRejected);

    EpollSlotDispatchPrototype arena({.capacity = 1, .maxNoticesPerBatch = 1});
    Target target;
    const auto identity = arena.registerOrUpdate(41, &target, kReadEvent).identity;
    std::atomic<bool> foreignRejected{false};
    std::thread foreign([&] {
        try {
            (void)arena.snapshot();
        } catch (const std::logic_error&) {
            foreignRejected.store(true, std::memory_order_release);
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(foreignRejected.load(std::memory_order_acquire));

    arena.beginStop();
    GAMENET_TEST_ASSERT(
        arena.registerOrUpdate(42, &target, kReadEvent).result ==
        SlotDispatchResult::Stopped);
    GAMENET_TEST_ASSERT(!arena.tryShutdown());
    GAMENET_TEST_ASSERT(arena.cancel(identity, &target) == SlotDispatchResult::Accepted);
    GAMENET_TEST_ASSERT(arena.tryShutdown());
    GAMENET_TEST_ASSERT(arena.snapshot().settled());
}

}  // namespace

int main() {
    testCapacityIdentityUpdateAndExactCancel();
    testO1DecodeMergeFilteringAndStaleTokens();
    testBatchBudgetEpochWrapAndActiveBatchInvalidation();
    testOwnerStopAndFailurePaths();
    return 0;
}
