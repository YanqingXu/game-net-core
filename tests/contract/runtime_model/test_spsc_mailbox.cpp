// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp2/SpscMailbox.h"

#include "support/TestAssert.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gamenet::experimental::hp2::MailboxDrainStatus;
using gamenet::experimental::hp2::MailboxPushStatus;
using gamenet::experimental::hp2::SpscMailbox;
using gamenet::experimental::hp2::makeSpscMailboxMatrixPlan;

struct MoveProbe {
    explicit MoveProbe(std::uint64_t value) noexcept : value(value) {}

    MoveProbe(const MoveProbe&) = delete;
    MoveProbe& operator=(const MoveProbe&) = delete;

    MoveProbe(MoveProbe&& other) noexcept : value(other.value) {
        other.movedFrom = true;
    }

    MoveProbe& operator=(MoveProbe&&) = delete;

    std::uint64_t value{};
    bool movedFrom{false};
};

void testCapacityAndMatrixBounds() {
    bool zeroRejected = false;
    try {
        SpscMailbox<MoveProbe> invalid(0);
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    GAMENET_TEST_ASSERT(zeroRejected);

    bool nonPowerOfTwoRejected = false;
    try {
        SpscMailbox<MoveProbe> invalid(3);
    } catch (const std::invalid_argument&) {
        nonPowerOfTwoRejected = true;
    }
    GAMENET_TEST_ASSERT(nonPowerOfTwoRejected);

    const auto plan = makeSpscMailboxMatrixPlan(4, 3, 2, 64, 32, 64 * 1024);
    GAMENET_TEST_ASSERT(plan.mailboxCount == 24);
    GAMENET_TEST_ASSERT(plan.slotCount == 1'536);
    GAMENET_TEST_ASSERT(plan.totalSlotBytes == 49'152);

    bool budgetRejected = false;
    try {
        (void)makeSpscMailboxMatrixPlan(4, 3, 2, 64, 32, 49'151);
    } catch (const std::length_error&) {
        budgetRejected = true;
    }
    GAMENET_TEST_ASSERT(budgetRejected);

    bool overflowRejected = false;
    try {
        (void)makeSpscMailboxMatrixPlan(
            (std::numeric_limits<std::size_t>::max)(), 2, 1, 2, 1,
            (std::numeric_limits<std::size_t>::max)());
    } catch (const std::overflow_error&) {
        overflowRejected = true;
    }
    GAMENET_TEST_ASSERT(overflowRejected);
}

void testMoveOnlyFifoWrapAndTypedRejection() {
    SpscMailbox<MoveProbe> mailbox(4);
    for (std::uint64_t value = 0; value < 4; ++value) {
        MoveProbe probe(value);
        GAMENET_TEST_ASSERT(mailbox.tryPush(std::move(probe)) == MailboxPushStatus::Accepted);
        GAMENET_TEST_ASSERT(probe.movedFrom);
    }

    MoveProbe fullProbe(99);
    GAMENET_TEST_ASSERT(
        mailbox.tryPush(std::move(fullProbe)) == MailboxPushStatus::QueueFull);
    GAMENET_TEST_ASSERT(!fullProbe.movedFrom);

    std::vector<std::uint64_t> observed;
    auto firstDrain = mailbox.drain(2, [&](MoveProbe value) {
        observed.push_back(value.value);
    });
    GAMENET_TEST_ASSERT(firstDrain.status == MailboxDrainStatus::Drained);
    GAMENET_TEST_ASSERT(firstDrain.processed == 2);
    GAMENET_TEST_ASSERT(firstDrain.remaining == 2);
    GAMENET_TEST_ASSERT(firstDrain.needsContinuation);

    for (std::uint64_t value = 4; value < 6; ++value) {
        MoveProbe probe(value);
        GAMENET_TEST_ASSERT(mailbox.tryPush(std::move(probe)) == MailboxPushStatus::Accepted);
    }
    const auto secondDrain = mailbox.drain(8, [&](MoveProbe value) {
        observed.push_back(value.value);
    });
    GAMENET_TEST_ASSERT(secondDrain.processed == 4);
    GAMENET_TEST_ASSERT(!secondDrain.needsContinuation);
    GAMENET_TEST_ASSERT((observed == std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5}));

    mailbox.beginStop();
    MoveProbe stoppedProbe(100);
    GAMENET_TEST_ASSERT(
        mailbox.tryPush(std::move(stoppedProbe)) == MailboxPushStatus::Stopped);
    GAMENET_TEST_ASSERT(!stoppedProbe.movedFrom);

    const auto snapshot = mailbox.snapshot();
    GAMENET_TEST_ASSERT(snapshot.accepted == 6);
    GAMENET_TEST_ASSERT(snapshot.processed == 6);
    GAMENET_TEST_ASSERT(snapshot.rejectedFull == 1);
    GAMENET_TEST_ASSERT(snapshot.rejectedStopped == 1);
    GAMENET_TEST_ASSERT(snapshot.depthHighWatermark == 4);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testVisitorExceptionAndExplicitCancellation() {
    SpscMailbox<MoveProbe> mailbox(8);
    for (std::uint64_t value = 0; value < 4; ++value) {
        MoveProbe probe(value);
        GAMENET_TEST_ASSERT(mailbox.tryPush(std::move(probe)) == MailboxPushStatus::Accepted);
    }

    auto result = mailbox.drain(8, [](MoveProbe value) {
        if (value.value == 1) throw std::runtime_error("contained visitor failure");
    });
    GAMENET_TEST_ASSERT(result.status == MailboxDrainStatus::VisitorException);
    GAMENET_TEST_ASSERT(result.processed == 1);
    GAMENET_TEST_ASSERT(result.cancelled == 1);
    GAMENET_TEST_ASSERT(result.remaining == 2);

    std::vector<std::uint64_t> cancelled;
    result = mailbox.cancelRemaining([&](MoveProbe value) {
        cancelled.push_back(value.value);
        if (value.value == 2) throw std::runtime_error("cancellation observer failure");
    });
    GAMENET_TEST_ASSERT(result.cancelled == 2);
    GAMENET_TEST_ASSERT((cancelled == std::vector<std::uint64_t>{2, 3}));

    const auto snapshot = mailbox.snapshot();
    GAMENET_TEST_ASSERT(snapshot.accepted == 4);
    GAMENET_TEST_ASSERT(snapshot.processed == 1);
    GAMENET_TEST_ASSERT(snapshot.cancelled == 3);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testConcurrentProducerConsumerOrdering() {
    constexpr std::uint64_t itemCount = 100'000;
    SpscMailbox<MoveProbe> mailbox(256);
    std::atomic<bool> producerDone{false};
    std::vector<std::uint64_t> observed;
    observed.reserve(itemCount);

    std::thread producer([&] {
        for (std::uint64_t value = 0; value < itemCount; ++value) {
            MoveProbe probe(value);
            while (mailbox.tryPush(std::move(probe)) == MailboxPushStatus::QueueFull) {
                GAMENET_TEST_ASSERT(!probe.movedFrom);
                std::this_thread::yield();
            }
            GAMENET_TEST_ASSERT(probe.movedFrom);
        }
        producerDone.store(true, std::memory_order_release);
    });

    while (!producerDone.load(std::memory_order_acquire) || !mailbox.empty()) {
        (void)mailbox.drain(31, [&](MoveProbe value) {
            observed.push_back(value.value);
        });
        std::this_thread::yield();
    }
    producer.join();

    GAMENET_TEST_ASSERT(observed.size() == itemCount);
    for (std::uint64_t index = 0; index < itemCount; ++index) {
        GAMENET_TEST_ASSERT(observed[index] == index);
    }
    const auto snapshot = mailbox.snapshot();
    GAMENET_TEST_ASSERT(snapshot.accepted == itemCount);
    GAMENET_TEST_ASSERT(snapshot.processed == itemCount);
    GAMENET_TEST_ASSERT(snapshot.cancelled == 0);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

}  // namespace

int main() {
    testCapacityAndMatrixBounds();
    testMoveOnlyFifoWrapAndTypedRejection();
    testVisitorExceptionAndExplicitCancellation();
    testConcurrentProducerConsumerOrdering();
    return 0;
}
