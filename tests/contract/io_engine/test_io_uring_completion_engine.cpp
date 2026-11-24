// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/experimental/io_uring/IoUringCompletionEngine.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
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
    OwnedFd(OwnedFd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
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

struct Listener {
    OwnedFd fd;
    sockaddr_in address{};
};

Listener makeListener() {
    OwnedFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    GAMENET_TEST_ASSERT(fd.get() >= 0);
    int enabled = 1;
    GAMENET_TEST_ASSERT(
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    GAMENET_TEST_ASSERT(
        ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    GAMENET_TEST_ASSERT(::listen(fd.get(), 8) == 0);
    socklen_t length = sizeof(address);
    GAMENET_TEST_ASSERT(
        ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    return {.fd = std::move(fd), .address = address};
}

OwnedFd connectClient(const sockaddr_in& address) {
    OwnedFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    GAMENET_TEST_ASSERT(fd.get() >= 0);
    GAMENET_TEST_ASSERT(
        ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

uring::IoUringCompletionNotice waitForNotice(
    uring::IoUringCompletionEngine& engine,
    uring::IoUringOperationKind expectedKind) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)engine.wait(50ms);
        while (auto notice = engine.takeNextNotice()) {
            if (notice->kind() == expectedKind) return std::move(*notice);
        }
    }
    throw std::runtime_error("timed out waiting for io_uring notice");
}

void testInvalidOptionsRejected() {
    gamenet::net::EventLoop loop;
    bool rejected = false;
    try {
        uring::IoUringCompletionEngine engine(
            &loop,
            uring::IoUringCompletionEngineOptions{.entries = 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GAMENET_TEST_ASSERT(rejected);
}

void testForeignThreadDestructionFailsFast() {
    const pid_t child = ::fork();
    GAMENET_TEST_ASSERT(child >= 0);
    if (child == 0) {
        std::set_terminate([] { ::_exit(86); });
        ::alarm(2);
        gamenet::net::EventLoop loop;
        auto engine = std::make_unique<uring::IoUringCompletionEngine>(&loop);
        GAMENET_TEST_ASSERT(
            engine->shutdown(0ms) == uring::IoUringShutdownResult::Drained);
        std::thread foreign([engine = std::move(engine)]() mutable {
            engine.reset();
        });
        foreign.join();
        ::_exit(87);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    GAMENET_TEST_ASSERT(waited == child);
    GAMENET_TEST_ASSERT(WIFEXITED(status));
    GAMENET_TEST_ASSERT(WEXITSTATUS(status) == 86);
}

void testForeignThreadMutationRejected() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(&loop);
    bool rejected = false;
    std::thread foreign([&] {
        try {
            (void)engine.enqueueRecv(-1, 1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    });
    foreign.join();
    GAMENET_TEST_ASSERT(rejected);
    GAMENET_TEST_ASSERT(engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
}

void testFiniteSqRejectsWithoutFallback() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 2,
            .maxOperations = 4,
            .maxCompletionsPerWait = 2,
            .maxBytesPerOperation = 16,
            .maxOwnedBytes = 64,
        });
    std::array<int, 2> pair{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    OwnedFd reader(pair[0]);
    OwnedFd writer(pair[1]);

    const auto first = engine.enqueueRecv(reader.get(), 1);
    const auto second = engine.enqueueRecv(reader.get(), 1);
    const auto rejected = engine.enqueueRecv(reader.get(), 1);
    GAMENET_TEST_ASSERT(first.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(second.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(
        rejected.result == uring::IoUringSubmissionResult::SubmissionQueueFull);
    GAMENET_TEST_ASSERT(engine.metrics().sqFullRejections == 1);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);

    const std::array bytes{'a', 'b'};
    GAMENET_TEST_ASSERT(::send(writer.get(), bytes.data(), bytes.size(), 0) == 2);
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (received != 2 && std::chrono::steady_clock::now() < deadline) {
        (void)engine.wait(50ms);
        while (auto notice = engine.takeNextNotice()) {
            GAMENET_TEST_ASSERT(
                notice->status() == uring::IoUringCompletionStatus::Succeeded);
            GAMENET_TEST_ASSERT(notice->payload().size() == 1);
            ++received;
        }
    }
    GAMENET_TEST_ASSERT(received == 2);
    GAMENET_TEST_ASSERT(engine.metrics().crossDomainFallbacks == 0);
    GAMENET_TEST_ASSERT(engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
}

void testOneShotAcceptRecvSend() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 8,
            .maxOperations = 8,
            .maxCompletionsPerWait = 4,
            .maxBytesPerOperation = 1024,
            .maxOwnedBytes = 8192,
        });
    auto listener = makeListener();
    const auto accepted = engine.enqueueAccept(listener.fd.get());
    GAMENET_TEST_ASSERT(accepted.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    auto client = connectClient(listener.address);
    auto acceptNotice = waitForNotice(engine, uring::IoUringOperationKind::Accept);
    GAMENET_TEST_ASSERT(
        acceptNotice.status() == uring::IoUringCompletionStatus::Succeeded);
    OwnedFd server(acceptNotice.releaseAcceptedSocket());
    GAMENET_TEST_ASSERT(server.get() >= 0);

    auto recvLease = std::make_shared<int>(7);
    std::weak_ptr<int> observedRecvLease = recvLease;
    const auto recv = engine.enqueueRecv(server.get(), 64, recvLease);
    recvLease.reset();
    GAMENET_TEST_ASSERT(recv.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(!observedRecvLease.expired());
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    constexpr std::string_view ping = "ping";
    GAMENET_TEST_ASSERT(::send(client.get(), ping.data(), ping.size(), 0) == 4);
    {
        auto recvNotice = waitForNotice(engine, uring::IoUringOperationKind::Receive);
        GAMENET_TEST_ASSERT(
            recvNotice.status() == uring::IoUringCompletionStatus::Succeeded);
        GAMENET_TEST_ASSERT(recvNotice.payload() == ping);
        GAMENET_TEST_ASSERT(recvNotice.identity() == recv.identity);
        GAMENET_TEST_ASSERT(!observedRecvLease.expired());
    }
    GAMENET_TEST_ASSERT(observedRecvLease.expired());

    constexpr std::string_view pong = "pong";
    const auto send = engine.enqueueSend(server.get(), pong);
    GAMENET_TEST_ASSERT(send.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    auto sendNotice = waitForNotice(engine, uring::IoUringOperationKind::Send);
    GAMENET_TEST_ASSERT(
        sendNotice.status() == uring::IoUringCompletionStatus::Succeeded);
    GAMENET_TEST_ASSERT(sendNotice.bytesTransferred() == pong.size());
    std::array<char, 4> response{};
    GAMENET_TEST_ASSERT(::recv(client.get(), response.data(), response.size(), 0) == 4);
    GAMENET_TEST_ASSERT(std::string_view(response.data(), response.size()) == pong);
    GAMENET_TEST_ASSERT(engine.metrics().crossDomainFallbacks == 0);
    GAMENET_TEST_ASSERT(engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
}

void testOneShotConnectCopiesAddressAndRetainsLease() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 8,
            .maxOperations = 8,
            .maxCompletionsPerWait = 8,
            .maxBytesPerOperation = 32,
            .maxOwnedBytes = 128,
        });
    auto listener = makeListener();
    OwnedFd socket(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_TCP));
    GAMENET_TEST_ASSERT(socket.get() >= 0);
    gamenet::net::InetAddress address(listener.address);
    auto lease = std::make_shared<int>(13);
    std::weak_ptr<int> observedLease = lease;
    const auto connect = engine.enqueueConnect(socket.get(), address, lease);
    lease.reset();
    address = gamenet::net::InetAddress(1, true);
    GAMENET_TEST_ASSERT(
        connect.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    GAMENET_TEST_ASSERT(!observedLease.expired());

    auto notice = waitForNotice(engine, uring::IoUringOperationKind::Connect);
    GAMENET_TEST_ASSERT(notice.identity() == connect.identity);
    GAMENET_TEST_ASSERT(
        notice.status() == uring::IoUringCompletionStatus::Succeeded);
    GAMENET_TEST_ASSERT(notice.bytesTransferred() == 0);
    OwnedFd accepted(::accept4(
        listener.fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    GAMENET_TEST_ASSERT(accepted.get() >= 0);
    GAMENET_TEST_ASSERT(!observedLease.expired());
    notice = {};
    GAMENET_TEST_ASSERT(observedLease.expired());
    GAMENET_TEST_ASSERT(
        engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
    GAMENET_TEST_ASSERT(engine.metrics().activeOperations == 0);
    GAMENET_TEST_ASSERT(engine.metrics().readyNotices == 0);
}

void testSlotGenerationRejectsStaleCancel() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 2,
            .maxOperations = 1,
            .maxCompletionsPerWait = 2,
            .maxBytesPerOperation = 8,
            .maxOwnedBytes = 8,
        });
    std::array<int, 2> pair{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    OwnedFd reader(pair[0]);
    OwnedFd writer(pair[1]);

    const auto first = engine.enqueueRecv(reader.get(), 1);
    GAMENET_TEST_ASSERT(first.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    const char byte = 'g';
    GAMENET_TEST_ASSERT(::send(writer.get(), &byte, 1, 0) == 1);
    {
        auto notice = waitForNotice(engine, uring::IoUringOperationKind::Receive);
        GAMENET_TEST_ASSERT(notice.identity() == first.identity);
    }

    const auto second = engine.enqueueRecv(reader.get(), 1);
    GAMENET_TEST_ASSERT(second.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(second.identity.slot == first.identity.slot);
    GAMENET_TEST_ASSERT(second.identity.generation != first.identity.generation);
    GAMENET_TEST_ASSERT(
        engine.cancel(first.identity) == uring::IoUringCancelResult::RejectedInvalid);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    GAMENET_TEST_ASSERT(
        engine.cancel(second.identity) == uring::IoUringCancelResult::Accepted);
    GAMENET_TEST_ASSERT(
        engine.cancel(second.identity) == uring::IoUringCancelResult::AlreadyRequested);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    GAMENET_TEST_ASSERT(engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
    auto notice = engine.takeNextNotice();
    GAMENET_TEST_ASSERT(notice.has_value());
    GAMENET_TEST_ASSERT(notice->identity() == second.identity);
    GAMENET_TEST_ASSERT(
        notice->status() == uring::IoUringCompletionStatus::Cancelled);
}

void testTerminalNoticeRetainsOperationSlotGeneration() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 8,
            .maxOperations = 3,
            .maxCompletionsPerWait = 1,
            .maxBytesPerOperation = 4,
            .maxOwnedBytes = 12,
        });
    std::array<int, 2> pairA{};
    std::array<int, 2> pairB{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pairA.data()) == 0);
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pairB.data()) == 0);
    OwnedFd readerA(pairA[0]);
    OwnedFd writerA(pairA[1]);
    OwnedFd readerB(pairB[0]);
    OwnedFd writerB(pairB[1]);

    const auto first = engine.enqueueRecv(readerA.get(), 1);
    const auto second = engine.enqueueRecv(readerB.get(), 1);
    GAMENET_TEST_ASSERT(first.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(second.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    const char incoming = 'A';
    GAMENET_TEST_ASSERT(
        ::send(writerA.get(), &incoming, 1, MSG_NOSIGNAL) == 1);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (engine.metrics().readyNotices == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)engine.wait(50ms);
    }
    GAMENET_TEST_ASSERT(engine.metrics().readyNotices == 1);

    const auto third = engine.enqueueSend(writerB.get(), "B");
    GAMENET_TEST_ASSERT(third.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(third.identity.slot != first.identity.slot);
    auto firstNotice = engine.takeNextNotice();
    GAMENET_TEST_ASSERT(firstNotice.has_value());
    GAMENET_TEST_ASSERT(firstNotice->identity() == first.identity);
    GAMENET_TEST_ASSERT(firstNotice->payload() == "A");

    GAMENET_TEST_ASSERT(
        engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
    while (engine.takeNextNotice()) {
    }
    GAMENET_TEST_ASSERT(engine.metrics().activeOperations == 0);
    GAMENET_TEST_ASSERT(engine.metrics().readyNotices == 0);
    GAMENET_TEST_ASSERT(engine.metrics().ownedBytes == 0);
}

void testFullTableReuseRetainsTakenNoticePayloadAndLease() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(&loop, {
        .entries = 8, .maxOperations = 4, .maxCompletionsPerWait = 8,
        .maxBytesPerOperation = 1, .maxOwnedBytes = 4,
    });
    std::array<int, 2> pair{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    OwnedFd reader(pair[0]);
    OwnedFd writer(pair[1]);
    std::array<uring::IoUringOperationIdentity, 4> previous{};
    std::vector<uring::IoUringCompletionNotice> retained;
    std::vector<std::weak_ptr<int>> leases;

    for (int round = 0; round < 16; ++round) {
        std::array<uring::IoUringOperationIdentity, 4> current{};
        for (std::size_t index = 0; index < current.size(); ++index) {
            auto lease = std::make_shared<int>(round);
            leases.push_back(lease);
            const auto submitted = engine.enqueueRecv(reader.get(), 1, lease);
            GAMENET_TEST_ASSERT(submitted.result == uring::IoUringSubmissionResult::Accepted);
            current[index] = submitted.identity;
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                GAMENET_TEST_ASSERT(current[earlier].slot != current[index].slot);
            }
        }
        for (const auto old : previous) {
            if (old.valid()) {
                GAMENET_TEST_ASSERT(engine.cancel(old) == uring::IoUringCancelResult::RejectedInvalid);
            }
        }
        GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
        GAMENET_TEST_ASSERT(::send(writer.get(), "qqqq", 4, MSG_NOSIGNAL) == 4);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (engine.metrics().readyNotices != 4 &&
               std::chrono::steady_clock::now() < deadline) {
            (void)engine.wait(50ms);
        }
        GAMENET_TEST_ASSERT(engine.metrics().readyNotices == 4);
        GAMENET_TEST_ASSERT(engine.metrics().activeOperations == 0);
        // Decoding alone must not put any of these four slots on the free chain.
        GAMENET_TEST_ASSERT(engine.enqueueRecv(reader.get(), 1).result ==
                            uring::IoUringSubmissionResult::OperationLimit);
        for (std::size_t index = 0; index < current.size(); ++index) {
            auto notice = engine.takeNextNotice();
            GAMENET_TEST_ASSERT(notice.has_value());
            retained.push_back(std::move(*notice));
        }
        GAMENET_TEST_ASSERT(!engine.takeNextNotice());
        previous = current;
    }
    GAMENET_TEST_ASSERT(engine.shutdown(0ms) == uring::IoUringShutdownResult::Drained);
    GAMENET_TEST_ASSERT(engine.metrics().ownedBytes == 0);
    for (std::size_t index = 0; index < retained.size(); ++index) {
        GAMENET_TEST_ASSERT(retained[index].payload() == "q");
        GAMENET_TEST_ASSERT(!leases[index].expired());
    }
    retained.clear();
    for (const auto& lease : leases) GAMENET_TEST_ASSERT(lease.expired());
}

void testSendSnapshotSurvivesCallerMutationAndSlotReuse() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(&loop, {
        .entries = 2, .maxOperations = 1, .maxCompletionsPerWait = 2,
        .maxBytesPerOperation = 4096, .maxOwnedBytes = 4096,
    });
    std::array<int, 2> pair{};
    GAMENET_TEST_ASSERT(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    OwnedFd sender(pair[0]);
    OwnedFd receiver(pair[1]);
    std::vector<uring::IoUringCompletionNotice> retained;
    std::vector<std::weak_ptr<int>> leases;
    for (const auto size : {1U, 31U, 4096U}) {
        std::string expected(size, '\0');
        for (std::size_t i = 0; i < size; ++i) expected[i] = static_cast<char>(i % 256);
        auto caller = expected;
        auto lease = std::make_shared<int>(1);
        leases.push_back(lease);
        const auto sent = engine.enqueueSend(sender.get(), caller, lease);
        lease.reset();
        GAMENET_TEST_ASSERT(sent.result == uring::IoUringSubmissionResult::Accepted);
        caller.assign(size, 'x');
        GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
        auto notice = waitForNotice(engine, uring::IoUringOperationKind::Send);
        GAMENET_TEST_ASSERT(notice.status() == uring::IoUringCompletionStatus::Succeeded);
        GAMENET_TEST_ASSERT(notice.payload() == expected);
        GAMENET_TEST_ASSERT(notice.bytesTransferred() == size);
        std::string actual(size, '\0');
        GAMENET_TEST_ASSERT(::recv(receiver.get(), actual.data(), size, MSG_WAITALL) == size);
        GAMENET_TEST_ASSERT(actual == expected);
        retained.push_back(std::move(notice));
    }
    GAMENET_TEST_ASSERT(engine.shutdown(0ms) == uring::IoUringShutdownResult::Drained);
    for (std::size_t n = 0; n < retained.size(); ++n) {
        GAMENET_TEST_ASSERT(!leases[n].expired());
        for (std::size_t i = 0; i < retained[n].payload().size(); ++i) {
            GAMENET_TEST_ASSERT(retained[n].payload()[i] == static_cast<char>(i % 256));
        }
    }
    retained.clear();
    for (const auto& lease : leases) GAMENET_TEST_ASSERT(lease.expired());
    GAMENET_TEST_ASSERT(engine.metrics().ownedBytes == 0);
}

void testCancelLeaseAndFinalDrain() {
    gamenet::net::EventLoop loop;
    uring::IoUringCompletionEngine engine(
        &loop,
        uring::IoUringCompletionEngineOptions{
            .entries = 4,
            .maxOperations = 4,
            .maxCompletionsPerWait = 4,
            .maxBytesPerOperation = 64,
            .maxOwnedBytes = 256,
        });
    std::array<int, 2> pair{};
    GAMENET_TEST_ASSERT(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    OwnedFd reader(pair[0]);
    OwnedFd writer(pair[1]);
    auto lease = std::make_shared<int>(11);
    std::weak_ptr<int> observedLease = lease;
    const auto recv = engine.enqueueRecv(reader.get(), 32, lease);
    lease.reset();
    GAMENET_TEST_ASSERT(recv.result == uring::IoUringSubmissionResult::Accepted);
    GAMENET_TEST_ASSERT(engine.flush().nativeError == 0);
    GAMENET_TEST_ASSERT(!observedLease.expired());

    engine.beginQuiesce();
    GAMENET_TEST_ASSERT(
        engine.enqueueRecv(reader.get(), 1).result ==
        uring::IoUringSubmissionResult::RejectedQuiescing);
    GAMENET_TEST_ASSERT(engine.shutdown(2s) == uring::IoUringShutdownResult::Drained);
    GAMENET_TEST_ASSERT(engine.quiescent());
    GAMENET_TEST_ASSERT(engine.phase() == uring::IoUringPhase::Shutdown);
    GAMENET_TEST_ASSERT(!observedLease.expired());

    auto notice = engine.takeNextNotice();
    GAMENET_TEST_ASSERT(notice.has_value());
    GAMENET_TEST_ASSERT(notice->identity() == recv.identity);
    GAMENET_TEST_ASSERT(
        notice->status() == uring::IoUringCompletionStatus::Cancelled);
    notice.reset();
    GAMENET_TEST_ASSERT(observedLease.expired());
    const auto metrics = engine.metrics();
    GAMENET_TEST_ASSERT(metrics.cancelRequests == 1);
    GAMENET_TEST_ASSERT(metrics.cancelCqes == 1);
    GAMENET_TEST_ASSERT(metrics.cancelledOperations == 1);
    GAMENET_TEST_ASSERT(metrics.terminalNotices == 1);
    GAMENET_TEST_ASSERT(metrics.activeOperations == 0);
    GAMENET_TEST_ASSERT(metrics.pendingSubmissions == 0);
    GAMENET_TEST_ASSERT(metrics.crossDomainFallbacks == 0);
}

}  // namespace

int main() {
    testInvalidOptionsRejected();
    testForeignThreadDestructionFailsFast();
    testForeignThreadMutationRejected();
    testFiniteSqRejectsWithoutFallback();
    testOneShotAcceptRecvSend();
    testOneShotConnectCopiesAddressAndRetainsLease();
    testSlotGenerationRejectsStaleCancel();
    testTerminalNoticeRetainsOperationSlotGeneration();
    testFullTableReuseRetainsTakenNoticePayloadAndLease();
    testSendSnapshotSurvivesCallerMutationAndSlotReuse();
    testCancelLeaseAndFinalDrain();
    return 0;
}
