#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/EventLoopMetrics.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"

#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopControlRegistry.h"
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"
#include "support/FutureTest.h"
#include "support/TestAssert.h"

#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#endif

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

namespace {

using EngineHarness = gamenet::net::detail::EventLoopControlRegistry;
using ActiveBatchHarness =
    gamenet::net::detail::EventLoopActiveBatchHarness;
using gamenet::net::detail::IoEngineAdmissionResult;
using gamenet::net::detail::IoEngineCapability;
using gamenet::net::detail::IoEngineOperationResult;
using gamenet::net::detail::IoEnginePhase;
using gamenet::net::detail::hasCapability;

struct SocketPair {
    gamenet::net::SocketFd first{gamenet::net::kInvalidSocket};
    gamenet::net::SocketFd second{gamenet::net::kInvalidSocket};

    SocketPair() {
#ifdef _WIN32
        gamenet::net::SocketFd fds[2]{
            gamenet::net::kInvalidSocket,
            gamenet::net::kInvalidSocket,
        };
        gamenet::net::sockets::createSocketPairOrDie(fds);
        first = fds[0];
        second = fds[1];
#else
        int fds[2];
        GAMENET_TEST_ASSERT(
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        first = fds[0];
        second = fds[1];
#endif
    }

    ~SocketPair() {
        gamenet::net::sockets::close(first);
        gamenet::net::sockets::close(second);
    }
};

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
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxIocpCompletionsPerPoll = 3,
    });
    GAMENET_TEST_ASSERT(hasExpectedPlatformCapabilities(loop));
    GAMENET_TEST_ASSERT(
        EngineHarness::ioEngineOptions(loop).
            maxCompletionNoticesPerWait == 3);
    GAMENET_TEST_ASSERT(
        EngineHarness::ioEngineAdmission(loop) ==
        IoEngineAdmissionResult::Accepted);
    GAMENET_TEST_ASSERT(
        EngineHarness::ioEnginePhase(loop) == IoEnginePhase::Running);
    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));

    loop.quit();
    loop.loop();

    GAMENET_TEST_ASSERT(
        EngineHarness::ioEnginePhase(loop) == IoEnginePhase::Quiescing);
    GAMENET_TEST_ASSERT(
        EngineHarness::ioEngineAdmission(loop) ==
        IoEngineAdmissionResult::RejectedQuiescing);
    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));
}

void testMutationRejectsForeignThreadAndInvalidIdentity() {
    gamenet::net::EventLoop loop;
    bool rejectedForeignThread = false;
    std::thread foreign([&] {
        try {
            (void)EngineHarness::updateIoEngineReadiness(loop, nullptr);
        } catch (const std::runtime_error&) {
            rejectedForeignThread = true;
        }
    });
    foreign.join();

    GAMENET_TEST_ASSERT(rejectedForeignThread);
    GAMENET_TEST_ASSERT(
        EngineHarness::updateIoEngineReadiness(loop, nullptr) ==
        IoEngineOperationResult::RejectedInvalid);
    GAMENET_TEST_ASSERT(
        EngineHarness::cancelIoEngineReadiness(loop, nullptr) ==
        IoEngineOperationResult::RejectedInvalid);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            nullptr,
            {}) == IoEngineOperationResult::RejectedInvalid);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionCancellation(
            loop,
            nullptr) == IoEngineOperationResult::RejectedInvalid);
}

void testCompletionCommitDrainsBeforeQuiesceReturns() {
    gamenet::net::EventLoop loop;

#ifdef _WIN32
    gamenet::net::IocpOperation operation{};
    operation.kind = gamenet::net::IocpOperationKind::Read;
    operation.channel = nullptr;
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> observedLifetime = lifetime;

    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            &operation,
            lifetime) == IoEngineOperationResult::Accepted);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionCancellation(
            loop,
            &operation) == IoEngineOperationResult::Accepted);
    lifetime.reset();
    GAMENET_TEST_ASSERT(!observedLifetime.expired());
    GAMENET_TEST_ASSERT(
        gamenet::net::detail::EventLoopIocpAssociationHarness::
            postCompletion(loop, &operation, 0));
#else
    int operation = 0;
    auto lifetime = std::make_shared<int>(7);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionSubmission(
            loop,
            &operation,
            lifetime) == IoEngineOperationResult::RejectedUnsupported);
    GAMENET_TEST_ASSERT(
        EngineHarness::commitIoEngineCompletionCancellation(
            loop,
            &operation) == IoEngineOperationResult::RejectedUnsupported);
#endif

    loop.quit();
    loop.loop();

    GAMENET_TEST_ASSERT(EngineHarness::ioEngineQuiescent(loop));
#ifdef _WIN32
    GAMENET_TEST_ASSERT(observedLifetime.expired());
    GAMENET_TEST_ASSERT(operation.completionObserved);
    GAMENET_TEST_ASSERT(!operation.shutdownObligation);
#endif
}

void testBudgetedDispatchContainsCloseAndStaleNotice() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxActiveChannelsPerIteration = 1,
    });
    std::array<SocketPair, 3> sockets;
    gamenet::net::Channel first(&loop, sockets[0].first);
    gamenet::net::Channel stale(&loop, sockets[1].first);
    gamenet::net::Channel third(&loop, sockets[2].first);
    std::array<int, 3> callbacks{0, 0, 0};
    std::vector<gamenet::net::EventLoopMetricSample> dispatchSamples;
    std::vector<gamenet::net::EventLoopCallbackSource> failures;

    loop.setCallbackExceptionHandler(
        [&](const gamenet::net::EventLoopCallbackException& failure) {
            failures.push_back(failure.source);
            return gamenet::net::EventLoopCallbackExceptionAction::Continue;
        });
    loop.setEventLoopMetricCallback(
        [&](const gamenet::net::EventLoopMetricSample& sample) {
            if (sample.event ==
                gamenet::net::EventLoopMetricEvent::ActiveChannelsDrained) {
                dispatchSamples.push_back(sample);
            }
        });

    first.setReadCallback([&](gamenet::base::Timestamp) {
        ++callbacks[0];
        first.disableAll();
        first.remove();
        stale.disableAll();
        stale.remove();
#ifdef _WIN32
        gamenet::net::detail::EventLoopIocpAssociationHarness::
            preserveSocketAssociation(loop, sockets[1].first);
#endif
        stale.enableReading();
        throw std::runtime_error("contained close callback failure");
    });
    stale.setReadCallback(
        [&](gamenet::base::Timestamp) { ++callbacks[1]; });
    third.setReadCallback(
        [&](gamenet::base::Timestamp) { ++callbacks[2]; });

    first.enableReading();
    stale.enableReading();
    third.enableReading();
    first.setRevents(gamenet::net::Channel::kReadEvent);
    stale.setRevents(gamenet::net::Channel::kReadEvent);
    third.setRevents(gamenet::net::Channel::kReadEvent);

    ActiveBatchHarness::install(
        loop,
        std::vector<gamenet::net::Channel*>{&first, &stale, &third},
        gamenet::base::now());
    ActiveBatchHarness::dispatchRound(loop);

    GAMENET_TEST_ASSERT((callbacks == std::array<int, 3>{1, 0, 0}));
    GAMENET_TEST_ASSERT(ActiveBatchHarness::pendingCount(loop) == 1);
    GAMENET_TEST_ASSERT(failures.size() == 1);
    GAMENET_TEST_ASSERT(
        failures.front() ==
        gamenet::net::EventLoopCallbackSource::ChannelEvent);
    GAMENET_TEST_ASSERT(dispatchSamples.size() == 1);
    GAMENET_TEST_ASSERT(dispatchSamples[0].drainedWork == 1);
    GAMENET_TEST_ASSERT(dispatchSamples[0].remainingWork == 1);
    GAMENET_TEST_ASSERT(dispatchSamples[0].budgetExhausted);

    ActiveBatchHarness::dispatchRound(loop);
    GAMENET_TEST_ASSERT((callbacks == std::array<int, 3>{1, 0, 1}));
    GAMENET_TEST_ASSERT(ActiveBatchHarness::pendingCount(loop) == 0);
    GAMENET_TEST_ASSERT(dispatchSamples.size() == 2);
    GAMENET_TEST_ASSERT(dispatchSamples[1].drainedWork == 1);
    GAMENET_TEST_ASSERT(dispatchSamples[1].remainingWork == 0);
    GAMENET_TEST_ASSERT(!dispatchSamples[1].budgetExhausted);

    stale.disableAll();
    stale.remove();
    third.disableAll();
    third.remove();
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
    testMutationRejectsForeignThreadAndInvalidIdentity();
    testCompletionCommitDrainsBeforeQuiesceReturns();
    testBudgetedDispatchContainsCloseAndStaleNotice();
    testCrossThreadWakeupDispatchesThroughAdapter();
    return 0;
}
