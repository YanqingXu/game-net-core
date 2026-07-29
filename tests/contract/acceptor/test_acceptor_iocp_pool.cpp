#include "support/TestAssert.h"

#ifdef _WIN32

#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"

#include "../../../src/core/net/detail/AcceptorIocpHarness.h"
#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"
#include "support/ClientSocket.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using gamenet::net::detail::EventLoopIocpAssociationHarness;

namespace {

void assertBackendQuiet(gamenet::net::EventLoop& loop) {
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) == 0);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::retainedCompletionCount(loop) == 0);
}

void assertPoolStorageReleased() {
    const auto observations =
        gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
    GAMENET_TEST_ASSERT(observations.currentSlots == 0);
    GAMENET_TEST_ASSERT(observations.currentSlotSockets == 0);
    GAMENET_TEST_ASSERT(observations.currentSubmitted == 0);
    GAMENET_TEST_ASSERT(observations.submissions == observations.completions);
}

void testFixedPoolBurstAndStopReentry() {
    constexpr std::size_t kDepth = 8;
    constexpr std::size_t kClientCount = 48;

    gamenet::net::detail::resetIocpAcceptPoolObservationsForTesting();
    gamenet::net::EventLoop loop;
    auto acceptor = std::make_unique<gamenet::net::Acceptor>(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    acceptor->setIocpAcceptDepth(kDepth);

    std::size_t accepted = 0;
    acceptor->setNewConnectionCallback(
        [&](gamenet::net::SocketFd socket,
            const gamenet::net::InetAddress&) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(socket));
            gamenet::test::closeTestSocket(socket);
            ++accepted;

            // Acceptor replenishes the exact completed slot before callback
            // entry, so a burst keeps the configured number pre-posted.
            const auto active =
                gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
            GAMENET_TEST_ASSERT(active.currentSubmitted == kDepth);
            GAMENET_TEST_ASSERT(active.currentSlotSockets == kDepth);

            if (accepted == kClientCount) {
                // Re-enter listener lifecycle from the accepted-socket
                // callback after the replacement generation was submitted.
                acceptor->stop();
                loop.quit();
            }
        });
    acceptor->listen();

    {
        const auto initial =
            gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
        GAMENET_TEST_ASSERT(initial.currentSlots == kDepth);
        GAMENET_TEST_ASSERT(initial.peakSlots == kDepth);
        GAMENET_TEST_ASSERT(initial.currentSlotSockets == kDepth);
        GAMENET_TEST_ASSERT(initial.currentSubmitted == kDepth);
        GAMENET_TEST_ASSERT(initial.peakSubmitted == kDepth);
        GAMENET_TEST_ASSERT(initial.submissions == kDepth);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) ==
            kDepth);
    }

    const auto listenAddress = acceptor->listenAddress();
    std::thread clients([listenAddress] {
        std::array<gamenet::net::SocketFd, kClientCount> sockets{};
        for (auto& socket : sockets) {
            socket = gamenet::test::connectTestClient(listenAddress);
        }
        for (const auto socket : sockets) {
            gamenet::test::closeTestSocket(socket);
        }
    });

    loop.runAfter(3s, [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for AcceptEx burst");
        loop.quit();
    });
    loop.loop();
    clients.join();

    GAMENET_TEST_ASSERT(acceptor);
    acceptor.reset();
    GAMENET_TEST_ASSERT(accepted == kClientCount);
    assertBackendQuiet(loop);
    assertPoolStorageReleased();

    const auto final =
        gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
    GAMENET_TEST_ASSERT(final.peakSlots == kDepth);
    GAMENET_TEST_ASSERT(final.peakSlotSockets == kDepth);
    GAMENET_TEST_ASSERT(final.peakSubmitted == kDepth);
    GAMENET_TEST_ASSERT(final.maxGeneration > 1);
    GAMENET_TEST_ASSERT(final.cancellationRequests == kDepth);
}

void testSynchronousFailureCancelsGenerationBeforeRetry() {
    constexpr std::size_t kDepth = 4;
    constexpr std::size_t kInitiallySubmitted = 2;

    gamenet::net::detail::resetIocpAcceptPoolObservationsForTesting();
    gamenet::net::EventLoop loop;
    auto acceptor = std::make_unique<gamenet::net::Acceptor>(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    acceptor->setIocpAcceptDepth(kDepth);

    std::size_t errors = 0;
    acceptor->setErrorCallback(
        [&](const gamenet::net::AcceptorError& failure) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                failure.stage == gamenet::net::AcceptorErrorStage::Accept);
            GAMENET_TEST_ASSERT(failure.errorCode == WSAENOBUFS);
            ++errors;
            return gamenet::net::AcceptorErrorAction::Retry;
        });

    gamenet::net::detail::injectIocpAcceptSubmissionErrorForTesting(
        kInitiallySubmitted,
        WSAENOBUFS);
    acceptor->listen();

    // Only the two real kernel submissions own leases and cancellation
    // obligations. The injected synchronous failure owns neither.
    GAMENET_TEST_ASSERT(errors == 1);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::retainedCompletionCount(loop) ==
        kInitiallySubmitted);
    GAMENET_TEST_ASSERT(
        EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) ==
        kInitiallySubmitted);
    {
        const auto retrying =
            gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
        GAMENET_TEST_ASSERT(retrying.currentSlots == kDepth);
        GAMENET_TEST_ASSERT(retrying.currentSubmitted == kInitiallySubmitted);
        GAMENET_TEST_ASSERT(
            retrying.cancellationRequests == kInitiallySubmitted);
        GAMENET_TEST_ASSERT(retrying.submissions == kInitiallySubmitted);
    }

    loop.runAfter(250ms, [&] {
        const auto resumed =
            gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
        GAMENET_TEST_ASSERT(errors == 1);
        GAMENET_TEST_ASSERT(resumed.currentSlots == kDepth);
        GAMENET_TEST_ASSERT(resumed.currentSlotSockets == kDepth);
        GAMENET_TEST_ASSERT(resumed.currentSubmitted == kDepth);
        GAMENET_TEST_ASSERT(resumed.peakSubmitted == kDepth);
        GAMENET_TEST_ASSERT(resumed.submissions == kInitiallySubmitted + kDepth);
        GAMENET_TEST_ASSERT(resumed.maxGeneration > 1);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::outstandingCompletionCount(loop) ==
            0);
        GAMENET_TEST_ASSERT(
            EventLoopIocpAssociationHarness::retainedCompletionCount(loop) ==
            kDepth);

        acceptor->stop();
        loop.quit();
    });
    loop.runAfter(2s, [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for AcceptEx retry");
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(acceptor);
    acceptor.reset();
    assertBackendQuiet(loop);
    assertPoolStorageReleased();
    const auto final =
        gamenet::net::detail::iocpAcceptPoolObservationsForTesting();
    GAMENET_TEST_ASSERT(final.peakSlots == kDepth);
    GAMENET_TEST_ASSERT(final.peakSlotSockets == kDepth);
    GAMENET_TEST_ASSERT(
        final.cancellationRequests == kInitiallySubmitted + kDepth);
}

}  // namespace

#endif

int main() {
#ifdef _WIN32
    testFixedPoolBurstAndStopReentry();
    testSynchronousFailureCancelsGenerationBeforeRetry();
#endif
    return 0;
}
