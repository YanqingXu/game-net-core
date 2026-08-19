#include "experimental/io_uring/IoUringTcpConnectionDriver.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    OwnedFd driver;
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
    OwnedFd driver(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
    GAMENET_TEST_ASSERT(driver.get() >= 0);
    return {std::move(driver), std::move(peer)};
}

uring::IoUringTcpConnectionDriverOptions driverOptions() {
    return {
        .pump = {
            .engine = {
                .entries = 16,
                .maxOperations = 4,
                .maxCompletionsPerWait = 8,
                .maxBytesPerOperation = 4,
                .maxOwnedBytes = 16,
            },
            .maxNoticesPerTurn = 1,
        },
        .maxReceiveBytes = 4,
        .maxSendBytesPerOperation = 3,
        .maxPendingSendBytes = 8,
        .maxPendingSendSegments = 2,
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

void testInvalidConstructionClosesTransferredSocket() {
    gamenet::net::EventLoop loop;
    auto pair = makeTcpPair();
    const int transferred = pair.driver.release();
    auto invalid = driverOptions();
    invalid.maxReceiveBytes = 0;
    bool rejected = false;
    try {
        uring::IoUringTcpConnectionDriver driver(
            &loop,
            transferred,
            invalid,
            [](std::string_view) {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GAMENET_TEST_ASSERT(rejected);
    errno = 0;
    GAMENET_TEST_ASSERT(::fcntl(transferred, F_GETFD) == -1);
    GAMENET_TEST_ASSERT(errno == EBADF);
}

void testFiniteSendAndReentrantPauseResume() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxPendingFunctors = 4,
        .reservedPendingFunctors = 0,
        .maxFunctorsPerIteration = 1,
        .maxControlSources = 0,
        .maxLifecycleNodes = 1,
        .maxLifecycleCallbacksPerIteration = 1,
        .maxActiveChannelsPerIteration = 1,
    });
    auto pair = makeTcpPair();
    uring::IoUringTcpConnectionDriver* driverPointer = nullptr;
    std::vector<std::string> messages;
    bool closeObservedStopped = false;
    bool duplicateCloseRejected = false;
    bool sendAfterStopRejected = false;
    bool timedOut = false;

    uring::IoUringTcpConnectionDriver driver(
        &loop,
        pair.driver.release(),
        driverOptions(),
        [&](std::string_view payload) {
            messages.emplace_back(payload);
            if (messages.size() == 1) {
                GAMENET_TEST_ASSERT(
                    driverPointer->pauseRead() ==
                    uring::IoUringTcpDriverReadControlResult::Applied);
                const char second = 'B';
                GAMENET_TEST_ASSERT(
                    ::send(pair.peer.get(), &second, 1, MSG_NOSIGNAL) == 1);
            } else {
                GAMENET_TEST_ASSERT(messages.size() == 2);
                GAMENET_TEST_ASSERT(driverPointer->close());
            }
        },
        [&](uring::IoUringTcpDriverCloseReason reason) {
            GAMENET_TEST_ASSERT(
                reason == uring::IoUringTcpDriverCloseReason::Explicit);
            closeObservedStopped =
                driverPointer->phase() == uring::IoUringTcpDriverPhase::Stopped;
            duplicateCloseRejected = !driverPointer->close();
            sendAfterStopRejected =
                driverPointer->send("late") ==
                uring::IoUringTcpDriverSendResult::RejectedClosing;
            loop.quit();
        });
    driverPointer = &driver;
    const auto stopped = driver.stopFuture();

    GAMENET_TEST_ASSERT(
        driver.start() == uring::IoUringTcpDriverStartResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.send("abcdefg") == uring::IoUringTcpDriverSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.send("hi") == uring::IoUringTcpDriverSendResult::ByteLimit);
    GAMENET_TEST_ASSERT(
        driver.send("h") == uring::IoUringTcpDriverSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.send("i") == uring::IoUringTcpDriverSendResult::SegmentLimit);

    const char first = 'A';
    GAMENET_TEST_ASSERT(
        ::send(pair.peer.get(), &first, 1, MSG_NOSIGNAL) == 1);
    loop.runAfter(25ms, [&] {
        GAMENET_TEST_ASSERT(messages.size() == 1);
        GAMENET_TEST_ASSERT(driver.metrics().receiveSubmissions == 1);
        GAMENET_TEST_ASSERT(
            driver.resumeRead() ==
            uring::IoUringTcpDriverReadControlResult::Applied);
    });
    loop.runAfter(2s, [&] {
        timedOut = true;
        driver.close();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(stopped.wait_for(0s) == std::future_status::ready);
    const auto summary = stopped.get();
    GAMENET_TEST_ASSERT(closeObservedStopped);
    GAMENET_TEST_ASSERT(duplicateCloseRejected);
    GAMENET_TEST_ASSERT(sendAfterStopRejected);
    GAMENET_TEST_ASSERT(messages == std::vector<std::string>({"A", "B"}));
    GAMENET_TEST_ASSERT(readToEof(pair.peer.get()) == "abcdefgh");
    GAMENET_TEST_ASSERT(
        summary.reason == uring::IoUringTcpDriverCloseReason::Explicit);
    GAMENET_TEST_ASSERT(summary.socketClosed);
    GAMENET_TEST_ASSERT(summary.driver.receiveSubmissions == 2);
    GAMENET_TEST_ASSERT(summary.driver.maxActiveReceives == 1);
    GAMENET_TEST_ASSERT(summary.driver.sendSubmissions == 4);
    GAMENET_TEST_ASSERT(summary.driver.sendAdmissions == 2);
    GAMENET_TEST_ASSERT(summary.driver.maxActiveSends == 1);
    GAMENET_TEST_ASSERT(summary.driver.bytesSent == 8);
    GAMENET_TEST_ASSERT(summary.driver.bytesDiscarded == 0);
    GAMENET_TEST_ASSERT(summary.driver.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.driver.pendingSendSegments == 0);
    GAMENET_TEST_ASSERT(summary.driver.maxPendingSendBytes == 8);
    GAMENET_TEST_ASSERT(summary.driver.maxPendingSendSegments == 2);
    GAMENET_TEST_ASSERT(summary.driver.byteLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.driver.segmentLimitRejections == 1);
    GAMENET_TEST_ASSERT(summary.driver.readPauses == 1);
    GAMENET_TEST_ASSERT(summary.driver.readResumes == 1);
    GAMENET_TEST_ASSERT(summary.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.ownedBytes == 0);
}

void testResumeWaitsForCancelledReceiveTerminal() {
    gamenet::net::EventLoop loop;
    auto pair = makeTcpPair();
    uring::IoUringTcpConnectionDriver* driverPointer = nullptr;
    std::size_t messages = 0;
    bool timedOut = false;

    uring::IoUringTcpConnectionDriver driver(
        &loop,
        pair.driver.release(),
        driverOptions(),
        [&](std::string_view payload) {
            GAMENET_TEST_ASSERT(payload == "C");
            ++messages;
            driverPointer->close();
        },
        [&](uring::IoUringTcpDriverCloseReason) { loop.quit(); });
    driverPointer = &driver;
    const auto stopped = driver.stopFuture();
    GAMENET_TEST_ASSERT(
        driver.start() == uring::IoUringTcpDriverStartResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.pauseRead() ==
        uring::IoUringTcpDriverReadControlResult::Applied);
    GAMENET_TEST_ASSERT(
        driver.resumeRead() ==
        uring::IoUringTcpDriverReadControlResult::Applied);

    loop.runAfter(25ms, [&] {
        const auto metrics = driver.metrics();
        GAMENET_TEST_ASSERT(metrics.receiveCancellations == 1);
        GAMENET_TEST_ASSERT(metrics.receiveSubmissions == 2);
        const char payload = 'C';
        GAMENET_TEST_ASSERT(
            ::send(pair.peer.get(), &payload, 1, MSG_NOSIGNAL) == 1);
    });
    loop.runAfter(2s, [&] {
        timedOut = true;
        driver.close();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(messages == 1);
    const auto summary = stopped.get();
    GAMENET_TEST_ASSERT(summary.driver.receiveCancellations == 1);
    GAMENET_TEST_ASSERT(summary.driver.receiveSubmissions == 2);
    GAMENET_TEST_ASSERT(summary.driver.maxActiveReceives == 1);
}

void testEventLoopQuitCancelsPendingReceiveAndRejectsForeignMutation() {
    gamenet::net::EventLoop loop;
    auto pair = makeTcpPair();
    uring::IoUringTcpConnectionDriver* driverPointer = nullptr;
    std::size_t closeCalls = 0;
    bool foreignRejected = false;

    uring::IoUringTcpConnectionDriver driver(
        &loop,
        pair.driver.release(),
        driverOptions(),
        [](std::string_view) {
            GAMENET_TEST_ASSERT(false);
        },
        [&](uring::IoUringTcpDriverCloseReason reason) {
            ++closeCalls;
            GAMENET_TEST_ASSERT(
                reason ==
                uring::IoUringTcpDriverCloseReason::EventLoopQuiescing);
            GAMENET_TEST_ASSERT(
                driverPointer->phase() == uring::IoUringTcpDriverPhase::Stopped);
        });
    driverPointer = &driver;
    const auto stopped = driver.stopFuture();
    GAMENET_TEST_ASSERT(
        driver.start() == uring::IoUringTcpDriverStartResult::Accepted);

    std::thread foreign([&] {
        try {
            (void)driver.send("foreign");
        } catch (const std::runtime_error&) {
            foreignRejected = true;
        }
    });
    foreign.join();
    loop.runAfter(1ms, [&] { loop.quit(); });
    loop.loop();

    GAMENET_TEST_ASSERT(foreignRejected);
    GAMENET_TEST_ASSERT(closeCalls == 1);
    const auto summary = stopped.get();
    GAMENET_TEST_ASSERT(
        summary.reason ==
        uring::IoUringTcpDriverCloseReason::EventLoopQuiescing);
    GAMENET_TEST_ASSERT(summary.driver.receiveCancellations == 1);
    GAMENET_TEST_ASSERT(summary.driver.foreignMutationRejections == 1);
    GAMENET_TEST_ASSERT(summary.driver.socketCloseCount == 1);
    GAMENET_TEST_ASSERT(summary.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.pendingCancelCompletions == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.pump.engine.ownedBytes == 0);
}

void testExplicitCloseAccountsAcceptedSendAndPendingReceive() {
    gamenet::net::EventLoop loop;
    auto pair = makeTcpPair();
    uring::IoUringTcpConnectionDriver driver(
        &loop,
        pair.driver.release(),
        driverOptions(),
        [](std::string_view) { GAMENET_TEST_ASSERT(false); },
        [&](uring::IoUringTcpDriverCloseReason reason) {
            GAMENET_TEST_ASSERT(
                reason == uring::IoUringTcpDriverCloseReason::Explicit);
            loop.quit();
        });
    const auto stopped = driver.stopFuture();
    GAMENET_TEST_ASSERT(
        driver.start() == uring::IoUringTcpDriverStartResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.send("abcdefg") == uring::IoUringTcpDriverSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        driver.send("h") == uring::IoUringTcpDriverSendResult::Accepted);
    GAMENET_TEST_ASSERT(driver.close());
    loop.loop();

    const auto summary = stopped.get();
    const auto delivered = readToEof(pair.peer.get());
    const std::string expected = "abcdefgh";
    GAMENET_TEST_ASSERT(
        delivered == expected.substr(0, delivered.size()));
    GAMENET_TEST_ASSERT(delivered.size() == summary.driver.bytesSent);
    GAMENET_TEST_ASSERT(
        summary.driver.bytesSent + summary.driver.bytesDiscarded == 8);
    GAMENET_TEST_ASSERT(summary.driver.receiveCancellations == 1);
    GAMENET_TEST_ASSERT(summary.driver.sendTerminals == 1);
    GAMENET_TEST_ASSERT(summary.driver.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.driver.pendingSendSegments == 0);
}

void testMessageFailureClosesWithoutEscapingPump() {
    gamenet::net::EventLoop loop;
    auto pair = makeTcpPair();
    uring::IoUringTcpConnectionDriver driver(
        &loop,
        pair.driver.release(),
        driverOptions(),
        [](std::string_view) {
            throw std::runtime_error("deterministic driver callback failure");
        },
        [&](uring::IoUringTcpDriverCloseReason reason) {
            GAMENET_TEST_ASSERT(
                reason == uring::IoUringTcpDriverCloseReason::CallbackFailed);
            loop.quit();
        });
    const auto stopped = driver.stopFuture();
    GAMENET_TEST_ASSERT(
        driver.start() == uring::IoUringTcpDriverStartResult::Accepted);
    const char payload = 'X';
    GAMENET_TEST_ASSERT(
        ::send(pair.peer.get(), &payload, 1, MSG_NOSIGNAL) == 1);
    loop.loop();

    const auto summary = stopped.get();
    GAMENET_TEST_ASSERT(
        summary.reason == uring::IoUringTcpDriverCloseReason::CallbackFailed);
    GAMENET_TEST_ASSERT(summary.driver.callbackFailures == 1);
    GAMENET_TEST_ASSERT(summary.driver.socketCloseCount == 1);
}

}  // namespace

int main() {
    testInvalidConstructionClosesTransferredSocket();
    testFiniteSendAndReentrantPauseResume();
    testResumeWaitsForCancelledReceiveTerminal();
    testEventLoopQuitCancelsPendingReceiveAndRejectsForeignMutation();
    testExplicitCloseAccountsAcceptedSendAndPendingReceive();
    testMessageFailureClosesWithoutEscapingPump();
    return 0;
}
