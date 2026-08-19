#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/EventLoopThread.h"

#include "../../../src/core/net/detail/EventLoopControlRegistry.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace {

using EngineHarness = gamenet::net::detail::EventLoopControlRegistry;
using gamenet::net::detail::IoEngineCapability;
using gamenet::net::detail::IoEnginePhase;
using gamenet::net::detail::hasCapability;

bool hasExpectedPlatformCapabilities(const gamenet::net::EventLoop& loop) {
    const auto capabilities = EngineHarness::ioEngineCapabilities(loop);
#ifdef _WIN32
    return hasCapability(capabilities, IoEngineCapability::Completion) &&
        hasCapability(capabilities, IoEngineCapability::BackendWakeup) &&
        !hasCapability(capabilities, IoEngineCapability::Readiness);
#else
    return hasCapability(capabilities, IoEngineCapability::Readiness) &&
        !hasCapability(capabilities, IoEngineCapability::Completion) &&
        !hasCapability(capabilities, IoEngineCapability::BackendWakeup);
#endif
}

void testAdapterLifecycleFollowsOwnerLoop() {
    gamenet::net::EventLoop loop;
    GAMENET_TEST_ASSERT(hasExpectedPlatformCapabilities(loop));
    GAMENET_TEST_ASSERT(
        EngineHarness::ioEnginePhase(loop) == IoEnginePhase::Running);
    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));

    loop.quit();
    loop.loop();

    GAMENET_TEST_ASSERT(
        EngineHarness::ioEnginePhase(loop) == IoEnginePhase::Quiescing);
    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));
}

void testCrossThreadWakeupDispatchesThroughAdapter() {
    gamenet::net::EventLoopThread loopThread;
    gamenet::net::EventLoop* loop = loopThread.startLoop();
    const auto executor = loop->executor();

    std::promise<bool> observed;
    auto future = observed.get_future();
    GAMENET_TEST_ASSERT(
        executor.post([loop, &observed] {
            const bool valid = loop->isInLoopThread() &&
                hasExpectedPlatformCapabilities(*loop) &&
                EngineHarness::ioEnginePhase(*loop) ==
                    IoEnginePhase::Running;
            observed.set_value(valid);
            loop->quit();
        }) == gamenet::net::PostResult::Accepted);

    gamenet::test::waitUntilReady(
        future,
        2s,
        "cross-thread Engine wakeup did not dispatch on the owner loop");
    GAMENET_TEST_ASSERT(future.get());
    loopThread.stop();
}

}  // namespace

int main() {
    testAdapterLifecycleFollowsOwnerLoop();
    testCrossThreadWakeupDispatchesThroughAdapter();
    return 0;
}
