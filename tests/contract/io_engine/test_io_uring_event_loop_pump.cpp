// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/experimental/io_uring/IoUringEventLoopPump.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

namespace uring = gamenet::experimental::io_uring;

class OwnedFd {
public:
    explicit OwnedFd(int value = -1) noexcept : value_(value) {}
    ~OwnedFd() {
        if (value_ >= 0) ::close(value_);
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this == &other) return *this;
        if (value_ >= 0) ::close(value_);
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    int get() const noexcept { return value_; }

private:
    int value_;
};

struct SocketPair {
    OwnedFd pump;
    OwnedFd peer;
};

SocketPair makeSocketPair() {
    std::array<int, 2> descriptors{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors.data()) == 0);
    return {OwnedFd(descriptors[0]), OwnedFd(descriptors[1])};
}

void testActiveDestructionFailsFastWithoutOwnerWait() {
    const auto started = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    GAMENET_TEST_ASSERT(child >= 0);
    if (child == 0) {
        std::set_terminate([] { ::_exit(86); });
        ::alarm(2);
        gamenet::net::EventLoop loop;
        auto pair = makeSocketPair();
        auto lease = std::make_shared<int>(7);
        std::weak_ptr<int> observedLease = lease;
        {
            uring::IoUringEventLoopPump pump(
                &loop,
                uring::IoUringEventLoopPumpOptions{
                    .engine = {
                        .entries = 8,
                        .maxOperations = 8,
                        .maxCompletionsPerWait = 8,
                        .maxBytesPerOperation = 64,
                        .maxOwnedBytes = 512,
                    },
                    .maxNoticesPerTurn = 1,
                },
                [](uring::IoUringCompletionNotice&) {});
            const auto outcome = pump.enqueueRecv(pair.pump.get(), 8, lease);
            lease.reset();
            GAMENET_TEST_ASSERT(
                outcome.result == uring::IoUringSubmissionResult::Accepted);
            GAMENET_TEST_ASSERT(!observedLease.expired());
        }
        ::_exit(87);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    GAMENET_TEST_ASSERT(waited == child);
    GAMENET_TEST_ASSERT(WIFEXITED(status));
    GAMENET_TEST_ASSERT(WEXITSTATUS(status) == 86);
    GAMENET_TEST_ASSERT(elapsed < 225ms);
}

void testRegistrationFailureRollsBackEngine() {
    gamenet::net::EventLoop loop;
    loop.quit();
    loop.loop();
    GAMENET_TEST_ASSERT(
        loop.phase() == gamenet::net::EventLoopPhase::Shutdown);

    bool registrationRejected = false;
    try {
        uring::IoUringEventLoopPump pump(
            &loop,
            uring::IoUringEventLoopPumpOptions{},
            [](uring::IoUringCompletionNotice&) {});
    } catch (const std::logic_error&) {
        registrationRejected = true;
    }
    GAMENET_TEST_ASSERT(registrationRejected);
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 0);
}

enum class SendAction { OutsideConsumer, NoRearm, Throw, Rearm, Twice, Cancel, Quiesce };

void testSendCommitBoundary(SendAction action, unsigned entries) {
    gamenet::net::EventLoop loop;
    auto pair = makeSocketPair();
    uring::IoUringEventLoopPump* pointer = nullptr;
    auto lease = std::make_shared<int>(9);
    const std::weak_ptr<int> observed = lease;
    const std::string expected("before\0after", 12);
    const std::size_t expectedSends = action == SendAction::Twice ? 2 : 1;
    std::size_t sends = 0;
    std::size_t receivedBytes = 0;
    bool timeout = false;
    auto admitSend = [&] {
        auto caller = expected;
        const auto outcome = pointer->enqueueSend(pair.pump.get(), caller, lease);
        GAMENET_TEST_ASSERT(outcome.result == uring::IoUringSubmissionResult::Accepted);
        caller.assign(caller.size(), 'x');
        return outcome.identity;
    };
    uring::IoUringEventLoopPump pump(&loop, {
        .engine = {.entries = entries, .maxOperations = 2, .maxCompletionsPerWait = 2,
                   .maxBytesPerOperation = 64, .maxOwnedBytes = 128},
        .maxNoticesPerTurn = 1,
    }, [&](uring::IoUringCompletionNotice& notice) {
        if (notice.kind() == uring::IoUringOperationKind::Send) {
            GAMENET_TEST_ASSERT(!observed.expired());
            GAMENET_TEST_ASSERT(notice.status() == uring::IoUringCompletionStatus::Succeeded ||
                                ((action == SendAction::Cancel || action == SendAction::Quiesce) &&
                                 notice.status() == uring::IoUringCompletionStatus::Cancelled));
            if (notice.status() == uring::IoUringCompletionStatus::Succeeded) {
                GAMENET_TEST_ASSERT(notice.payload() == expected);
                receivedBytes += notice.bytesTransferred();
            }
            if (++sends == expectedSends) loop.quit();
            return;
        }
        if (notice.status() == uring::IoUringCompletionStatus::Cancelled) return;
        GAMENET_TEST_ASSERT(notice.payload() == "a");
        const auto identity = admitSend();
        if (action == SendAction::Twice) {
            (void)admitSend();
            GAMENET_TEST_ASSERT(pointer->enqueueSend(pair.pump.get(), "overflow").result ==
                                uring::IoUringSubmissionResult::OperationLimit);
        }
        lease.reset();
        GAMENET_TEST_ASSERT(!observed.expired());
        if (action == SendAction::Rearm) {
            GAMENET_TEST_ASSERT(pointer->enqueueRecv(pair.pump.get(), 1).result ==
                                uring::IoUringSubmissionResult::Accepted);
        } else if (action == SendAction::Cancel) {
            // Must not become RejectedNotSubmitted because Send was staged.
            GAMENET_TEST_ASSERT(pointer->cancel(identity) == uring::IoUringCancelResult::Accepted);
        } else if (action == SendAction::Quiesce) {
            pointer->beginQuiesce();
            loop.quit();
        } else if (action == SendAction::Throw) {
            throw std::runtime_error("Send must commit on exceptional consumer exit");
        }
    });
    pointer = &pump;
    if (action == SendAction::OutsideConsumer) {
        (void)admitSend();
        lease.reset();
        timeval deadline{.tv_sec = 1, .tv_usec = 0};
        GAMENET_TEST_ASSERT(::setsockopt(pair.peer.get(), SOL_SOCKET, SO_RCVTIMEO,
                                       &deadline, sizeof(deadline)) == 0);
        std::array<char, 12> response{};
        // No EventLoop turn is required to commit non-callback Send.
        GAMENET_TEST_ASSERT(::recv(pair.peer.get(), response.data(), response.size(), MSG_WAITALL) == 12);
        GAMENET_TEST_ASSERT(std::string_view(response.data(), response.size()) == expected);
    } else {
        GAMENET_TEST_ASSERT(pump.enqueueRecv(pair.pump.get(), 1).result ==
                            uring::IoUringSubmissionResult::Accepted);
        GAMENET_TEST_ASSERT(::send(pair.peer.get(), "a", 1, MSG_NOSIGNAL) == 1);
    }
    loop.runAfter(2s, [&] { timeout = true; loop.quit(); });
    loop.loop();
    const auto result = pump.stopFuture().get();
    GAMENET_TEST_ASSERT(!timeout && sends == expectedSends && observed.expired());
    GAMENET_TEST_ASSERT(result.pump.maxConsumerDepth == 1 && result.pump.driveFailures == 0);
    GAMENET_TEST_ASSERT(result.pump.consumerFailures == (action == SendAction::Throw ? 1 : 0));
    GAMENET_TEST_ASSERT(result.engine.activeOperations == 0 && result.engine.readyNotices == 0 &&
                        result.engine.pendingSubmissions == 0 && result.engine.pendingCancelCompletions == 0 &&
                        result.engine.ownedBytes == 0 && result.engine.crossDomainFallbacks == 0);
    if (action != SendAction::OutsideConsumer && receivedBytes != 0) {
        std::array<char, 24> response{};
        GAMENET_TEST_ASSERT(::recv(pair.peer.get(), response.data(), receivedBytes, MSG_DONTWAIT) ==
                            static_cast<ssize_t>(receivedBytes));
        for (std::size_t i = 0; i < receivedBytes; ++i) {
            GAMENET_TEST_ASSERT(response[i] == expected[i % expected.size()]);
        }
    }
}

}  // namespace

int main() {
    testRegistrationFailureRollsBackEngine();
    testActiveDestructionFailsFastWithoutOwnerWait();
    for (const auto action : {SendAction::OutsideConsumer, SendAction::NoRearm,
                             SendAction::Throw, SendAction::Rearm, SendAction::Twice,
                             SendAction::Cancel, SendAction::Quiesce}) {
        testSendCommitBoundary(action, 2);
    }
    testSendCommitBoundary(SendAction::NoRearm, 1);
    testSendCommitBoundary(SendAction::Cancel, 1);
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 8,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 2,
        .maxControlSources = 0,
        .maxLifecycleNodes = 1,
        .maxLifecycleCallbacksPerIteration = 1,
        .maxActiveChannelsPerIteration = 1,
    });
    auto first = makeSocketPair();
    auto second = makeSocketPair();
    auto pending = makeSocketPair();

    auto pendingLease = std::make_shared<int>(41);
    std::weak_ptr<int> observedPendingLease = pendingLease;
    uring::IoUringEventLoopPump* pumpPointer = nullptr;
    std::size_t readyReceives = 0;
    std::size_t sentNotices = 0;
    std::size_t cancelledNotices = 0;
    int consumerDepth = 0;
    int consumerDepthPeak = 0;
    bool injectedConsumerFailure = false;
    bool stoppedHookCalled = false;
    bool stoppedHookObservedRetirement = false;
    bool timedOut = false;

    bool invalidOptionsRejected = false;
    try {
        uring::IoUringEventLoopPump invalid(
            &loop,
            uring::IoUringEventLoopPumpOptions{.maxNoticesPerTurn = 0},
            [](uring::IoUringCompletionNotice&) {});
    } catch (const std::invalid_argument&) {
        invalidOptionsRejected = true;
    }
    GAMENET_TEST_ASSERT(invalidOptionsRejected);

    uring::IoUringEventLoopPump pump(
        &loop,
        uring::IoUringEventLoopPumpOptions{
            .engine = {
                .entries = 8,
                .maxOperations = 8,
                .maxCompletionsPerWait = 8,
                .maxBytesPerOperation = 64,
                .maxOwnedBytes = 512,
            },
            .maxNoticesPerTurn = 1,
        },
        [&](uring::IoUringCompletionNotice& notice) {
            ++consumerDepth;
            consumerDepthPeak = (std::max)(consumerDepthPeak, consumerDepth);
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (notice.kind() == uring::IoUringOperationKind::Receive &&
                notice.status() == uring::IoUringCompletionStatus::Succeeded) {
                GAMENET_TEST_ASSERT(notice.payload() == "a");
                ++readyReceives;
                if (readyReceives == 2) {
                    const auto sent = pumpPointer->enqueueSend(first.pump.get(), "pong");
                    GAMENET_TEST_ASSERT(
                        sent.result == uring::IoUringSubmissionResult::Accepted);
                }
                if (!injectedConsumerFailure) {
                    injectedConsumerFailure = true;
                    --consumerDepth;
                    throw std::runtime_error("deterministic pump consumer failure");
                }
            } else if (notice.kind() == uring::IoUringOperationKind::Send) {
                GAMENET_TEST_ASSERT(
                    notice.status() == uring::IoUringCompletionStatus::Succeeded);
                GAMENET_TEST_ASSERT(notice.bytesTransferred() == 4);
                ++sentNotices;
                loop.quit();
            } else {
                GAMENET_TEST_ASSERT(
                    notice.kind() == uring::IoUringOperationKind::Receive);
                GAMENET_TEST_ASSERT(
                    notice.status() == uring::IoUringCompletionStatus::Cancelled);
                GAMENET_TEST_ASSERT(loop.phase() != gamenet::net::EventLoopPhase::Running);
                GAMENET_TEST_ASSERT(!observedPendingLease.expired());
                ++cancelledNotices;
            }
            --consumerDepth;
        },
        [&](const uring::IoUringEventLoopPumpStopSummary& stoppedSummary) {
            stoppedHookCalled = true;
            stoppedHookObservedRetirement =
                pumpPointer->stopped() &&
                loop.attachedLifecycleNodeCount() == 0 &&
                stoppedSummary.engine.activeOperations == 0 &&
                stoppedSummary.engine.pendingCancelCompletions == 0 &&
                stoppedSummary.engine.readyNotices == 0 &&
                stoppedSummary.engine.ownedBytes == 0;
        });
    pumpPointer = &pump;
    const auto stopFuture = pump.stopFuture();

    bool foreignMutationRejected = false;
    std::thread foreign([&] {
        try {
            (void)pump.enqueueRecv(-1, 1);
        } catch (const std::runtime_error&) {
            foreignMutationRejected = true;
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(foreignMutationRejected);

    const auto firstRecv = pump.enqueueRecv(first.pump.get(), 8);
    const auto secondRecv = pump.enqueueRecv(second.pump.get(), 8);
    const auto pendingRecv = pump.enqueueRecv(pending.pump.get(), 8, pendingLease);
    pendingLease.reset();
    GAMENET_TEST_ASSERT(
        firstRecv.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(
        secondRecv.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(
        pendingRecv.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(!observedPendingLease.expired());

    const char byte = 'a';
    GAMENET_TEST_ASSERT(::send(first.peer.get(), &byte, 1, 0) == 1);
    GAMENET_TEST_ASSERT(::send(second.peer.get(), &byte, 1, 0) == 1);
    loop.runAfter(2s, [&] {
        timedOut = true;
        pump.beginQuiesce();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(stopFuture.wait_for(0s) == std::future_status::ready);
    const auto summary = stopFuture.get();
    GAMENET_TEST_ASSERT(
        summary.result == uring::IoUringEventLoopPumpStopResult::DrainedAfterFailure);
    GAMENET_TEST_ASSERT(readyReceives == 2);
    GAMENET_TEST_ASSERT(sentNotices == 1);
    GAMENET_TEST_ASSERT(cancelledNotices == 1);
    GAMENET_TEST_ASSERT(consumerDepth == 0);
    GAMENET_TEST_ASSERT(consumerDepthPeak == 1);
    GAMENET_TEST_ASSERT(observedPendingLease.expired());
    GAMENET_TEST_ASSERT(summary.pump.noticesDispatched == 4);
    GAMENET_TEST_ASSERT(summary.pump.consumerFailures == 1);
    GAMENET_TEST_ASSERT(summary.pump.continuationSignals >= 1);
    GAMENET_TEST_ASSERT(summary.pump.quiesceActivations == 1);
    GAMENET_TEST_ASSERT(stoppedHookCalled);
    GAMENET_TEST_ASSERT(stoppedHookObservedRetirement);
    GAMENET_TEST_ASSERT(summary.engine.operationsAccepted == 4);
    GAMENET_TEST_ASSERT(summary.engine.terminalNotices == 4);
    GAMENET_TEST_ASSERT(summary.engine.cancelledOperations == 1);
    GAMENET_TEST_ASSERT(summary.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.engine.pendingSubmissions == 0);
    GAMENET_TEST_ASSERT(summary.engine.pendingCancelCompletions == 0);
    GAMENET_TEST_ASSERT(summary.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.engine.ownedBytes == 0);
    GAMENET_TEST_ASSERT(pump.stopped());
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 0);
    GAMENET_TEST_ASSERT(loop.pendingLifecycleNodeCount() == 0);

    std::array<char, 4> response{};
    GAMENET_TEST_ASSERT(
        ::recv(first.peer.get(), response.data(), response.size(), 0) == 4);
    GAMENET_TEST_ASSERT(std::string_view(response.data(), response.size()) == "pong");
    return 0;
}
