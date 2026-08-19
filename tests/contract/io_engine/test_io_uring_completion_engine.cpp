#include "experimental/io_uring/IoUringCompletionEngine.h"

#include "gamenet/core/net/EventLoop.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    testForeignThreadMutationRejected();
    testFiniteSqRejectsWithoutFallback();
    testOneShotAcceptRecvSend();
    testSlotGenerationRejectsStaleCancel();
    testCancelLeaseAndFinalDrain();
    return 0;
}
