// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerView.h"
#include "hp2/MailboxSource.h"

#include "support/TestAssert.h"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gamenet::experimental::hp1::OwnedPacket;
using gamenet::experimental::hp2::DataPlaneCommand;
using gamenet::experimental::hp2::MailboxDrainStatus;
using gamenet::experimental::hp2::MailboxPushStatus;
using gamenet::experimental::hp2::NotificationClearPhase;
using gamenet::experimental::hp2::SpscMailboxSource;

using Command = DataPlaneCommand<OwnedPacket>;

Command command(std::uint64_t id, std::uint64_t generation, std::string payload) {
    return Command(id, generation, 0, OwnedPacket(std::move(payload)));
}

void testBurstCoalescingBoundedDrainAndReentry() {
    std::atomic<std::uint64_t> notifications{0};
    SpscMailboxSource<Command> source(8, [&] {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    auto producer = source.producerHandle();

    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 7, "one")).status == MailboxPushStatus::Accepted);
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 7, "two")).status == MailboxPushStatus::Accepted);
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 7, "three")).status == MailboxPushStatus::Accepted);
    GAMENET_TEST_ASSERT(notifications.load(std::memory_order_relaxed) == 1);

    std::vector<std::string> observed;
    MailboxDrainStatus nestedStatus = MailboxDrainStatus::Drained;
    auto result = source.drain(2, [&](Command value) {
        observed.emplace_back(value.packet.asStringView());
        if (observed.size() == 1) {
            nestedStatus = source.drain(1, [](Command) {}).status;
        }
    });
    GAMENET_TEST_ASSERT(nestedStatus == MailboxDrainStatus::ReentrantDrainRejected);
    GAMENET_TEST_ASSERT(result.processed == 2);
    GAMENET_TEST_ASSERT(result.remaining == 1);
    GAMENET_TEST_ASSERT(result.needsContinuation);
    GAMENET_TEST_ASSERT(source.snapshot().notificationPending);

    result = source.drain(2, [&](Command value) {
        observed.emplace_back(value.packet.asStringView());
    });
    GAMENET_TEST_ASSERT(result.processed == 1);
    GAMENET_TEST_ASSERT(result.remaining == 0);
    GAMENET_TEST_ASSERT(!result.needsContinuation);
    GAMENET_TEST_ASSERT((observed == std::vector<std::string>{"one", "two", "three"}));

    const auto snapshot = source.snapshot();
    GAMENET_TEST_ASSERT(snapshot.physicalNotifications == 1);
    GAMENET_TEST_ASSERT(snapshot.mergedNotifications == 2);
    GAMENET_TEST_ASSERT(snapshot.ownerContinuations == 1);
    GAMENET_TEST_ASSERT(snapshot.reentrantDrainRejections == 1);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testProducerRaceBeforeClearSelfRearmsWithoutLostWork() {
    std::atomic<std::uint64_t> notifications{0};
    SpscMailboxSource<Command> source(8, [&] {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    auto producer = source.producerHandle();
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 1, "initial")).status == MailboxPushStatus::Accepted);

    std::atomic<bool> releaseProducer{false};
    std::atomic<bool> producerDone{false};
    MailboxPushStatus raceStatus = MailboxPushStatus::OwnerUnavailable;
    std::thread racingProducer([&] {
        while (!releaseProducer.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        raceStatus = producer.tryPush(command(1, 1, "before-clear")).status;
        producerDone.store(true, std::memory_order_release);
    });
    source.setClearHook([&](NotificationClearPhase phase) {
        if (phase != NotificationClearPhase::BeforeClear) return;
        releaseProducer.store(true, std::memory_order_release);
        while (!producerDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    auto result = source.drain(8, [](Command) {});
    racingProducer.join();
    source.setClearHook({});
    GAMENET_TEST_ASSERT(raceStatus == MailboxPushStatus::Accepted);
    GAMENET_TEST_ASSERT(result.remaining == 1);
    GAMENET_TEST_ASSERT(result.needsContinuation);
    GAMENET_TEST_ASSERT(notifications.load(std::memory_order_relaxed) == 1);

    result = source.drain(8, [](Command) {});
    GAMENET_TEST_ASSERT(result.processed == 1);
    const auto snapshot = source.snapshot();
    GAMENET_TEST_ASSERT(snapshot.selfRearms == 1);
    GAMENET_TEST_ASSERT(snapshot.physicalNotifications == 1);
    GAMENET_TEST_ASSERT(snapshot.mergedNotifications == 1);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testProducerRaceAfterClearUsesNewPhysicalNotification() {
    std::atomic<std::uint64_t> notifications{0};
    SpscMailboxSource<Command> source(8, [&] {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    auto producer = source.producerHandle();
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 1, "initial")).status == MailboxPushStatus::Accepted);

    std::atomic<bool> releaseProducer{false};
    std::atomic<bool> producerDone{false};
    std::thread racingProducer([&] {
        while (!releaseProducer.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        GAMENET_TEST_ASSERT(
            producer.tryPush(command(1, 1, "after-clear")).status ==
            MailboxPushStatus::Accepted);
        producerDone.store(true, std::memory_order_release);
    });
    source.setClearHook([&](NotificationClearPhase phase) {
        if (phase != NotificationClearPhase::AfterClear) return;
        releaseProducer.store(true, std::memory_order_release);
        while (!producerDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    auto result = source.drain(8, [](Command) {});
    racingProducer.join();
    source.setClearHook({});
    GAMENET_TEST_ASSERT(result.remaining == 1);
    GAMENET_TEST_ASSERT(!result.needsContinuation);
    GAMENET_TEST_ASSERT(notifications.load(std::memory_order_relaxed) == 2);

    result = source.drain(8, [](Command) {});
    GAMENET_TEST_ASSERT(result.processed == 1);
    const auto snapshot = source.snapshot();
    GAMENET_TEST_ASSERT(snapshot.selfRearms == 0);
    GAMENET_TEST_ASSERT(snapshot.ownerContinuations == 0);
    GAMENET_TEST_ASSERT(snapshot.physicalNotifications == 2);
    GAMENET_TEST_ASSERT(snapshot.settled());
}

void testLifecycleGenerationAndTerminalAccounting() {
    SpscMailboxSource<Command> source(2, [] {});
    auto producer = source.producerHandle();

    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 3, "one")).status == MailboxPushStatus::Accepted);
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 3, "two")).status == MailboxPushStatus::Accepted);
    auto rejected = command(1, 3, "full");
    GAMENET_TEST_ASSERT(
        producer.tryPush(std::move(rejected)).status == MailboxPushStatus::QueueFull);
    GAMENET_TEST_ASSERT(rejected.packet.asStringView() == "full");

    auto result = source.drain(1, [](Command) {
        throw std::runtime_error("contained source visitor failure");
    });
    GAMENET_TEST_ASSERT(result.status == MailboxDrainStatus::VisitorException);
    GAMENET_TEST_ASSERT(result.cancelled == 1);
    GAMENET_TEST_ASSERT(result.remaining == 1);

    auto stopped = command(1, 3, "stopped");
    GAMENET_TEST_ASSERT(
        producer.tryPush(std::move(stopped)).status == MailboxPushStatus::Stopped);
    GAMENET_TEST_ASSERT(stopped.packet.asStringView() == "stopped");
    result = source.cancelRemaining([](Command) {});
    GAMENET_TEST_ASSERT(result.cancelled == 1);
    GAMENET_TEST_ASSERT(source.snapshot().settled());

    const auto oldGeneration = producer.generation();
    GAMENET_TEST_ASSERT(source.detach());
    GAMENET_TEST_ASSERT(
        producer.tryPush(command(1, 3, "stale")).status ==
        MailboxPushStatus::OwnerUnavailable);

    SpscMailboxSource<Command> replacement(2, [] {});
    auto replacementProducer = replacement.producerHandle();
    GAMENET_TEST_ASSERT(replacementProducer.generation() != oldGeneration);
    GAMENET_TEST_ASSERT(
        replacementProducer.tryPush(command(1, 4, "replacement")).status ==
        MailboxPushStatus::Accepted);
    (void)replacement.drain(2, [](Command) {});
    GAMENET_TEST_ASSERT(replacement.snapshot().settled());
}

void testRouteAndOwnerOutboxGenerationValidation() {
    SpscMailboxSource<Command> input(4, [] {});
    SpscMailboxSource<Command> outbox(4, [] {});
    auto networkProducer = input.producerHandle();
    auto logicProducer = outbox.producerHandle();

    constexpr std::uint64_t routeId = 91;
    std::uint64_t currentGeneration = 7;
    GAMENET_TEST_ASSERT(
        networkProducer.tryPush(command(routeId, currentGeneration, "request")).status ==
        MailboxPushStatus::Accepted);

    std::size_t handled = 0;
    (void)input.drain(4, [&](Command value) {
        if (value.routeId != routeId || value.routeGeneration != currentGeneration) return;
        ++handled;
        GAMENET_TEST_ASSERT(
            logicProducer.tryPush(command(
                value.routeId, value.routeGeneration, "response")).status ==
            MailboxPushStatus::Accepted);
    });
    GAMENET_TEST_ASSERT(handled == 1);

    ++currentGeneration;  // Disconnect/replacement before network-owner send.
    std::size_t sent = 0;
    std::size_t stale = 0;
    (void)outbox.drain(4, [&](Command value) {
        if (value.routeId != routeId || value.routeGeneration != currentGeneration) {
            ++stale;
            return;
        }
        ++sent;
    });
    GAMENET_TEST_ASSERT(sent == 0);
    GAMENET_TEST_ASSERT(stale == 1);
    GAMENET_TEST_ASSERT(input.snapshot().settled());
    GAMENET_TEST_ASSERT(outbox.snapshot().settled());
}

}  // namespace

int main() {
    testBurstCoalescingBoundedDrainAndReentry();
    testProducerRaceBeforeClearSelfRearmsWithoutLostWork();
    testProducerRaceAfterClearUsesNewPhysicalNotification();
    testLifecycleGenerationAndTerminalAccounting();
    testRouteAndOwnerOutboxGenerationValidation();
    return 0;
}
