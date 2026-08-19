#include "experimental/io_uring/IoUringEventLoopPump.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
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

}  // namespace

int main() {
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
