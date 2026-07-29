#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopExecutor.h"

#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"
#include "support/TestAssert.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32

using gamenet::net::EventLoop;
using gamenet::net::EventLoopPhase;
using gamenet::net::detail::EventLoopIocpAssociationHarness;

std::atomic<bool> resetHookEntered{false};
std::atomic<bool> resetRaceProducerDone{false};

void blockAtWakeupReset() noexcept {
    resetHookEntered.store(true, std::memory_order_release);
    resetHookEntered.notify_all();
    while (!resetRaceProducerDone.load(std::memory_order_acquire)) {
        resetRaceProducerDone.wait(false, std::memory_order_acquire);
    }
}

void waitForResetHook() {
    while (!resetHookEntered.load(std::memory_order_acquire)) {
        resetHookEntered.wait(false, std::memory_order_acquire);
    }
}

void testMultiProducerBurstPostsOnePacket() {
    constexpr std::size_t kProducerCount = 8;
    constexpr std::size_t kWakeupsPerProducer = 512;

    EventLoop loop;
    EventLoopIocpAssociationHarness::resetWakeupObservations();

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&loop] {
            for (std::size_t request = 0;
                 request < kWakeupsPerProducer;
                 ++request) {
                loop.wakeup();
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    const auto logicalWakeups =
        static_cast<std::uint64_t>(
            kProducerCount * kWakeupsPerProducer);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::logicalWakeupCount(loop) ==
        logicalWakeups);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() == 1);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() <
        logicalWakeups);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::wakeupPending(loop));

    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::pollAndDispatch(loop) == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsConsumed() == 1);
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::wakeupPending(loop));
}

void testProducerAroundOwnerReset(bool producerAfterReset) {
    EventLoop loop;
    const auto executor = loop.executor();
    EventLoopIocpAssociationHarness::resetWakeupObservations();
    resetHookEntered.store(false, std::memory_order_release);
    resetRaceProducerDone.store(false, std::memory_order_release);

    if (producerAfterReset) {
        EventLoopIocpAssociationHarness::setWakeupResetHooks(
            nullptr,
            &blockAtWakeupReset);
    } else {
        EventLoopIocpAssociationHarness::setWakeupResetHooks(
            &blockAtWakeupReset,
            nullptr);
    }

    std::atomic<bool> accepted{false};
    bool acceptedWorkRan = false;
    std::thread producer([&] {
        waitForResetHook();
        accepted.store(
            executor.tryQueue([&] {
                acceptedWorkRan = true;
                loop.quit();
            }),
            std::memory_order_release);
        resetRaceProducerDone.store(true, std::memory_order_release);
        resetRaceProducerDone.notify_all();
    });

    loop.wakeup();
    loop.loop();
    producer.join();
    EventLoopIocpAssociationHarness::setWakeupResetHooks(nullptr, nullptr);

    GAMENET_TEST_ASSERT(accepted.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(acceptedWorkRan);
    GAMENET_TEST_ASSERT(loop.phase() == EventLoopPhase::Shutdown);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::logicalWakeupCount(loop) == 2);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() ==
        (producerAfterReset ? 2 : 1));
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsConsumed() ==
        (producerAfterReset ? 2 : 1));
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::wakeupPending(loop));
}

void testSelfRearmAndQuitDrainTheLastPacket() {
    EventLoop loop;
    const auto executor = loop.executor();
    EventLoopIocpAssociationHarness::resetWakeupObservations();

    bool firstWorkRan = false;
    bool rearmedWorkRan = false;
    std::atomic<bool> accepted{false};
    std::thread producer([&] {
        accepted.store(
            executor.tryQueue([&] {
                firstWorkRan = true;
                loop.queueInLoop([&] {
                    rearmedWorkRan = true;
                });
                loop.quit();
            }),
            std::memory_order_release);
    });
    producer.join();

    GAMENET_TEST_ASSERT(accepted.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() == 1);
    loop.loop();

    GAMENET_TEST_ASSERT(firstWorkRan);
    GAMENET_TEST_ASSERT(rearmedWorkRan);
    GAMENET_TEST_ASSERT(loop.phase() == EventLoopPhase::Shutdown);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::logicalWakeupCount(loop) == 3);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() == 2);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsConsumed() == 2);
    GAMENET_TEST_ASSERT(
        !EventLoopIocpAssociationHarness::wakeupPending(loop));

    const auto postedAtShutdown =
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted();
    GAMENET_TEST_ASSERT(!executor.tryQueue([] {}));
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::physicalWakeupPacketsPosted() ==
        postedAtShutdown);
}

#endif

}  // namespace

int main() {
#ifdef _WIN32
    testMultiProducerBurstPostsOnePacket();
    testProducerAroundOwnerReset(false);
    testProducerAroundOwnerReset(true);
    testSelfRearmAndQuitDrainTheLastPacket();
#endif
    return 0;
}
