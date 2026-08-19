#include "experimental/io_uring/IoUringTcpConnectionHub.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
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
    int release() noexcept { return std::exchange(value_, -1); }

private:
    int value_;
};

struct SocketPair {
    OwnedFd hub;
    OwnedFd peer;
};

SocketPair makeTcpPair() {
    OwnedFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    GAMENET_TEST_ASSERT(listener.get() >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    GAMENET_TEST_ASSERT(
        ::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0);
    GAMENET_TEST_ASSERT(::listen(listener.get(), 1) == 0);
    socklen_t addressLength = sizeof(address);
    GAMENET_TEST_ASSERT(
        ::getsockname(
            listener.get(),
            reinterpret_cast<sockaddr*>(&address),
            &addressLength) == 0);

    OwnedFd peer(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    GAMENET_TEST_ASSERT(peer.get() >= 0);
    GAMENET_TEST_ASSERT(
        ::connect(
            peer.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0);
    OwnedFd hub(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
    GAMENET_TEST_ASSERT(hub.get() >= 0);
    return {std::move(hub), std::move(peer)};
}

uring::IoUringTcpConnectionHubOptions hubOptions() {
    return {
        .pump = {
            .engine = {
                .entries = 32,
                .maxOperations = 8,
                .maxCompletionsPerWait = 8,
                .maxBytesPerOperation = 4,
                .maxOwnedBytes = 32,
            },
            .maxNoticesPerTurn = 1,
        },
        .maxConnections = 2,
        .maxTotalPendingSendBytes = 10,
        .maxReceiveBytes = 4,
        .maxSendBytesPerOperation = 3,
        .maxPendingSendBytesPerConnection = 8,
        .maxPendingSendSegmentsPerConnection = 2,
    };
}

std::string readToEof(int descriptor) {
    std::string result;
    std::array<char, 32> buffer{};
    while (true) {
        const auto count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            result.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        GAMENET_TEST_ASSERT(count == 0);
        return result;
    }
}

void sendByte(int descriptor, char value) {
    GAMENET_TEST_ASSERT(
        ::send(descriptor, &value, 1, MSG_NOSIGNAL) == 1);
}

void testSharedPumpIsolationGenerationAndReentrantReuse() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 8,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 0,
        .maxLifecycleNodes = 2,
        .maxLifecycleCallbacksPerIteration = 1,
        .maxActiveChannelsPerIteration = 1,
    });
    auto pairA = makeTcpPair();
    auto pairB = makeTcpPair();
    auto pairC = makeTcpPair();
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;
    uring::IoUringTcpConnectionIdentity identityA{};
    uring::IoUringTcpConnectionIdentity identityB{};
    uring::IoUringTcpConnectionIdentity identityC{};
    std::shared_future<uring::IoUringTcpConnectionHubConnectionStopSummary>
        futureA;
    std::shared_future<uring::IoUringTcpConnectionHubConnectionStopSummary>
        futureB;
    std::shared_future<uring::IoUringTcpConnectionHubConnectionStopSummary>
        futureC;
    std::size_t closeCalls = 0;
    bool aRetired = false;
    bool neighborProgressAfterA = false;
    bool oldIdentityRejected = false;
    bool timedOut = false;

    auto finishConnection = [&] {
        ++closeCalls;
        if (closeCalls == 3) {
            GAMENET_TEST_ASSERT(hubPointer->stop());
        }
    };

    uring::IoUringTcpConnectionHub hub(
        &loop,
        hubOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            GAMENET_TEST_ASSERT(summary.allConnectionsStopped);
            GAMENET_TEST_ASSERT(summary.hub.activeConnections == 0);
            GAMENET_TEST_ASSERT(summary.hub.activeOperationRoutes == 0);
            GAMENET_TEST_ASSERT(summary.hub.pendingSendBytes == 0);
            loop.quit();
        });
    hubPointer = &hub;

    const auto addedA = hub.addConnection(
        pairA.hub.release(),
        [&](uring::IoUringTcpConnectionIdentity identity, std::string_view payload) {
            GAMENET_TEST_ASSERT(identity == identityA);
            GAMENET_TEST_ASSERT(payload == "a");
            GAMENET_TEST_ASSERT(hub.closeConnection(identity));
        },
        [&](uring::IoUringTcpConnectionIdentity identity,
            uring::IoUringTcpHubCloseReason reason) {
            GAMENET_TEST_ASSERT(identity == identityA);
            GAMENET_TEST_ASSERT(reason == uring::IoUringTcpHubCloseReason::Explicit);
            GAMENET_TEST_ASSERT(futureA.wait_for(0s) == std::future_status::ready);
            aRetired = true;

            const auto addedC = hub.addConnection(
                pairC.hub.release(),
                [&](uring::IoUringTcpConnectionIdentity current,
                    std::string_view payload) {
                    GAMENET_TEST_ASSERT(current == identityC);
                    GAMENET_TEST_ASSERT(payload == "c");
                    throw std::runtime_error(
                        "deterministic shared-Hub callback failure");
                },
                [&](uring::IoUringTcpConnectionIdentity current,
                    uring::IoUringTcpHubCloseReason closeReason) {
                    GAMENET_TEST_ASSERT(current == identityC);
                    GAMENET_TEST_ASSERT(
                        closeReason ==
                        uring::IoUringTcpHubCloseReason::CallbackFailed);
                    finishConnection();
                });
            GAMENET_TEST_ASSERT(
                addedC.result == uring::IoUringTcpHubAddResult::Accepted);
            identityC = addedC.identity;
            futureC = addedC.stopFuture;
            GAMENET_TEST_ASSERT(identityC.slot == identityA.slot);
            GAMENET_TEST_ASSERT(identityC.generation != identityA.generation);
            oldIdentityRejected =
                hub.send(identityA, "late") ==
                    uring::IoUringTcpHubSendResult::StaleConnection &&
                !hub.closeConnection(identityA);
            GAMENET_TEST_ASSERT(
                hub.send(identityC, "C!") ==
                uring::IoUringTcpHubSendResult::Accepted);
            loop.runAfter(10ms, [&] {
                sendByte(pairB.peer.get(), 'b');
                sendByte(pairC.peer.get(), 'c');
            });
            finishConnection();
        });
    GAMENET_TEST_ASSERT(
        addedA.result == uring::IoUringTcpHubAddResult::Accepted);
    identityA = addedA.identity;
    futureA = addedA.stopFuture;

    const auto addedB = hub.addConnection(
        pairB.hub.release(),
        [&](uring::IoUringTcpConnectionIdentity identity, std::string_view payload) {
            GAMENET_TEST_ASSERT(identity == identityB);
            GAMENET_TEST_ASSERT(payload == "b");
            neighborProgressAfterA = aRetired;
            GAMENET_TEST_ASSERT(hub.closeConnection(identity));
        },
        [&](uring::IoUringTcpConnectionIdentity identity,
            uring::IoUringTcpHubCloseReason reason) {
            GAMENET_TEST_ASSERT(identity == identityB);
            GAMENET_TEST_ASSERT(reason == uring::IoUringTcpHubCloseReason::Explicit);
            finishConnection();
        });
    GAMENET_TEST_ASSERT(
        addedB.result == uring::IoUringTcpHubAddResult::Accepted);
    identityB = addedB.identity;
    futureB = addedB.stopFuture;
    GAMENET_TEST_ASSERT(
        hub.pauseRead(identityB) ==
        uring::IoUringTcpHubReadControlResult::Applied);
    GAMENET_TEST_ASSERT(
        hub.resumeRead(identityB) ==
        uring::IoUringTcpHubReadControlResult::Applied);

    GAMENET_TEST_ASSERT(
        hub.send(identityA, "abcdefg") ==
        uring::IoUringTcpHubSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(identityA, "hi") ==
        uring::IoUringTcpHubSendResult::ConnectionByteLimit);
    GAMENET_TEST_ASSERT(
        hub.send(identityA, "h") ==
        uring::IoUringTcpHubSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(identityA, "i") ==
        uring::IoUringTcpHubSendResult::ConnectionSegmentLimit);
    GAMENET_TEST_ASSERT(
        hub.send(identityB, "B!") ==
        uring::IoUringTcpHubSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(identityB, "x") ==
        uring::IoUringTcpHubSendResult::HubByteLimit);

    sendByte(pairA.peer.get(), 'a');
    loop.runAfter(2s, [&] {
        timedOut = true;
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(closeCalls == 3);
    GAMENET_TEST_ASSERT(neighborProgressAfterA);
    GAMENET_TEST_ASSERT(oldIdentityRejected);
    GAMENET_TEST_ASSERT(futureA.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(futureB.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(futureC.wait_for(0s) == std::future_status::ready);
    const auto summaryA = futureA.get();
    const auto summaryB = futureB.get();
    const auto summaryC = futureC.get();
    GAMENET_TEST_ASSERT(
        summaryA.connection.bytesSent + summaryA.connection.bytesDiscarded == 8);
    GAMENET_TEST_ASSERT(summaryB.connection.bytesSent == 2);
    GAMENET_TEST_ASSERT(summaryB.connection.bytesDiscarded == 0);
    GAMENET_TEST_ASSERT(summaryB.connection.receiveCancellations == 1);
    GAMENET_TEST_ASSERT(summaryB.connection.maxActiveReceives == 1);
    GAMENET_TEST_ASSERT(summaryC.connection.bytesSent == 2);
    GAMENET_TEST_ASSERT(summaryC.connection.bytesDiscarded == 0);
    GAMENET_TEST_ASSERT(
        summaryC.reason == uring::IoUringTcpHubCloseReason::CallbackFailed);
    GAMENET_TEST_ASSERT(readToEof(pairB.peer.get()) == "B!");
    GAMENET_TEST_ASSERT(readToEof(pairC.peer.get()) == "C!");
    const auto outputA = readToEof(pairA.peer.get());
    GAMENET_TEST_ASSERT(
        outputA == std::string("abcdefgh").substr(0, outputA.size()));
    GAMENET_TEST_ASSERT(outputA.size() == summaryA.connection.bytesSent);

    const auto summary = hub.stopFuture().get();
    GAMENET_TEST_ASSERT(hub.phase() == uring::IoUringTcpHubPhase::Stopped);
    GAMENET_TEST_ASSERT(summary.hub.connectionsAccepted == 3);
    GAMENET_TEST_ASSERT(summary.hub.connectionsRetired == 3);
    GAMENET_TEST_ASSERT(summary.hub.maxActiveConnections == 2);
    GAMENET_TEST_ASSERT(summary.hub.connectionByteLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.hub.connectionSegmentLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.hub.hubByteLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.hub.staleConnectionRejections == 2);
    GAMENET_TEST_ASSERT(summary.hub.callbackFailures == 1);
    GAMENET_TEST_ASSERT(summary.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.ownedBytes == 0);
}

void testCapacityForeignMutationAndAggregateEventLoopQuit() {
    gamenet::net::EventLoop loop;
    auto pairA = makeTcpPair();
    auto pairB = makeTcpPair();
    auto rejectedPair = makeTcpPair();
    std::size_t closeCalls = 0;
    std::atomic<bool> foreignRejected{false};
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;

    uring::IoUringTcpConnectionHub hub(&loop, hubOptions());
    hubPointer = &hub;
    auto onClose = [&](uring::IoUringTcpConnectionIdentity,
                       uring::IoUringTcpHubCloseReason reason) {
        ++closeCalls;
        GAMENET_TEST_ASSERT(
            reason == uring::IoUringTcpHubCloseReason::EventLoopQuiescing);
        GAMENET_TEST_ASSERT(
            hubPointer->phase() == uring::IoUringTcpHubPhase::Stopped);
    };
    const auto addedA = hub.addConnection(
        pairA.hub.release(),
        [](uring::IoUringTcpConnectionIdentity, std::string_view) {
            GAMENET_TEST_ASSERT(false);
        },
        onClose);
    const auto addedB = hub.addConnection(
        pairB.hub.release(),
        [](uring::IoUringTcpConnectionIdentity, std::string_view) {
            GAMENET_TEST_ASSERT(false);
        },
        onClose);
    GAMENET_TEST_ASSERT(
        addedA.result == uring::IoUringTcpHubAddResult::Accepted);
    GAMENET_TEST_ASSERT(
        addedB.result == uring::IoUringTcpHubAddResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(addedA.identity, "AA") ==
        uring::IoUringTcpHubSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(addedB.identity, "BB") ==
        uring::IoUringTcpHubSendResult::Accepted);

    const int rejectedFd = rejectedPair.hub.release();
    const auto rejected = hub.addConnection(
        rejectedFd,
        [](uring::IoUringTcpConnectionIdentity, std::string_view) {});
    GAMENET_TEST_ASSERT(
        rejected.result == uring::IoUringTcpHubAddResult::ConnectionLimit);
    errno = 0;
    GAMENET_TEST_ASSERT(::fcntl(rejectedFd, F_GETFD) == -1);
    GAMENET_TEST_ASSERT(errno == EBADF);

    std::thread foreign([&] {
        try {
            (void)hub.send(addedA.identity, "foreign");
        } catch (const std::runtime_error&) {
            foreignRejected.store(true, std::memory_order_relaxed);
        }
    });
    foreign.join();
    loop.runAfter(1ms, [&] { loop.quit(); });
    loop.loop();

    GAMENET_TEST_ASSERT(foreignRejected.load(std::memory_order_relaxed));
    GAMENET_TEST_ASSERT(closeCalls == 2);
    GAMENET_TEST_ASSERT(addedA.stopFuture.wait_for(0s) == std::future_status::ready);
    GAMENET_TEST_ASSERT(addedB.stopFuture.wait_for(0s) == std::future_status::ready);
    const auto stoppedA = addedA.stopFuture.get();
    const auto stoppedB = addedB.stopFuture.get();
    GAMENET_TEST_ASSERT(
        stoppedA.connection.bytesSent + stoppedA.connection.bytesDiscarded == 2);
    GAMENET_TEST_ASSERT(
        stoppedB.connection.bytesSent + stoppedB.connection.bytesDiscarded == 2);
    const auto summary = hub.stopFuture().get();
    GAMENET_TEST_ASSERT(summary.hub.connectionsAccepted == 2);
    GAMENET_TEST_ASSERT(summary.hub.connectionsRetired == 2);
    GAMENET_TEST_ASSERT(summary.hub.foreignMutationRejections == 1);
    GAMENET_TEST_ASSERT(summary.hub.socketCloseCount == 2);
    GAMENET_TEST_ASSERT(summary.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.pendingCancelCompletions == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.ownedBytes == 0);
}

}  // namespace

int main() {
    testSharedPumpIsolationGenerationAndReentrantReuse();
    testCapacityForeignMutationAndAggregateEventLoopQuit();
    return 0;
}
