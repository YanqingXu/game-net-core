// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/core/net/TcpConnection.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TimerId.h"

#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class PayloadAllocation { Normal, Pause, FailAfterPause };
thread_local PayloadAllocation nextPayloadAllocation = PayloadAllocation::Normal;
thread_local std::size_t minimumPausedAllocationBytes = 0;
std::atomic<bool> payloadAllocationPaused{false};
std::atomic<bool> resumePayloadAllocation{false};

}  // namespace

// Ignore small STL bookkeeping allocations: only the admitted payload can
// trigger the producer-only pause after reservation, before ownership/posting.
void* operator new(std::size_t bytes) {
    const auto action = bytes >= minimumPausedAllocationBytes
        ? nextPayloadAllocation : PayloadAllocation::Normal;
    if (action != PayloadAllocation::Normal) {
        nextPayloadAllocation = PayloadAllocation::Normal;
    }
    if (action != PayloadAllocation::Normal) {
        payloadAllocationPaused.store(true, std::memory_order_release);
        while (!resumePayloadAllocation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (action == PayloadAllocation::FailAfterPause) throw std::bad_alloc{};
    }
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

namespace {

void verifyOwnerSendThenForeignShutdown() {
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "cross-thread-shutdown-drains-output");

    const std::string payload(1024 * 1024, 'x');

    std::size_t peerReadBytes = 0;
    int connectedCallbackCount = 0;
    int disconnectedCallbackCount = 0;
    int writeCompleteCallbackCount = 0;
    int closeCallbackCount = 0;
    bool peerSawEof = false;
    bool shutdownIssued = false;
    gamenet::net::TimerId drainTimer;

    auto closeWhenContractObserved = [&] {
        if (peerSawEof && writeCompleteCallbackCount == 1) {
            loop.cancel(drainTimer);
            connection->forceClose();
        }
    };

    connection->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) {
            ++connectedCallbackCount;
            return;
        }
        ++disconnectedCallbackCount;
    });

    connection->setWriteCompleteCallback([&](const gamenet::net::TcpConnectionPtr&) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbackCount;
        closeWhenContractObserved();
    });

    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbackCount;
        conn->connectDestroyed();
        loop.quit();
    });

    drainTimer = loop.runEvery(std::chrono::milliseconds(1), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        char buffer[8192];
        while (true) {
            const ssize_t n = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (n > 0) {
                peerReadBytes += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                peerSawEof = true;
                closeWhenContractObserved();
                return;
            }

            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isWouldBlock(error) || gamenet::net::sockets::isInterrupted(error)) {
                return;
            }

            GAMENET_TEST_ASSERT(false && "unexpected peer read error while draining cross-thread shutdown output");
        }
    });

    loop.runAfter(std::chrono::milliseconds(0), [&] {
        auto conn = connection;
        conn->connectEstablished();
        GAMENET_TEST_ASSERT(conn->connected());
        conn->send(payload);

        // cross-thread-shutdown-drains-output: shutdown() from a non-owner
        // thread must marshal back, drain pending output, then half-close.
        gamenet::test::runFromNonOwnerThread([conn] {
            conn->shutdown();
        });
        shutdownIssued = true;
    });

    loop.runAfter(std::chrono::seconds(2), [&] {
        GAMENET_TEST_ASSERT(false && "timed out waiting for cross-thread shutdown output drain");
        loop.quit();
    });

    loop.loop();

    GAMENET_TEST_ASSERT(shutdownIssued);
    GAMENET_TEST_ASSERT(connectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbackCount == 1);
    GAMENET_TEST_ASSERT(writeCompleteCallbackCount == 1);
    GAMENET_TEST_ASSERT(peerReadBytes == payload.size());
    GAMENET_TEST_ASSERT(peerSawEof);
    GAMENET_TEST_ASSERT(closeCallbackCount == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());

}

enum class SendShutdownOrder { Sequential, Concurrent };
enum class NotificationAdmission { Available, Saturated };

void verifyQueuedSendBeforeHalfClose(
    std::size_t sendCount,
    std::size_t payloadBytes,
    SendShutdownOrder order,
    NotificationAdmission notifications = NotificationAdmission::Available) {
    using namespace gamenet::net;
    using namespace std::chrono_literals;
    const bool saturate = notifications == NotificationAdmission::Saturated;
    EventLoop loop(EventLoopOptions{
        .maxPendingFunctors = saturate ? 2U : 16U,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::test::ConnectedSocketPair pair(
        gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(
        loop, pair, "queued-send-before-graceful-half-close");

    std::vector<std::string> payloads;
    for (std::size_t index = 0; index < sendCount; ++index) {
        payloads.emplace_back(payloadBytes, static_cast<char>('a' + index));
    }
    std::string acceptedPayload;
    std::string receivedPayload;
    bool peerSawEof = false;
    unsigned connectedCallbacks = 0;
    unsigned disconnectedCallbacks = 0;
    unsigned closeCallbacks = 0;
    unsigned writeCompleteCallbacks = 0;
    PostResult shutdownResult = PostResult::OwnerUnavailable;

    connection->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        if (conn->connected()) ++connectedCallbacks;
        else ++disconnectedCallbacks;
    });
    connection->setWriteCompleteCallback([&](const TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++writeCompleteCallbacks;
        // Re-entry cannot reopen output or reissue a completed half-close.
        const auto expected = conn->disconnected()
            ? PostResult::Shutdown : PostResult::Accepted;
        GAMENET_TEST_ASSERT(conn->tryShutdown() == expected);
        GAMENET_TEST_ASSERT(conn->trySend("late callback") == TcpSendResult::Closed);
    });
    connection->setCloseCallback([&](const TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++closeCallbacks;
        const auto info = conn->closeInfo();
        GAMENET_TEST_ASSERT(info.has_value());
        GAMENET_TEST_ASSERT(info->reason == TcpConnectionCloseReason::GracefulShutdown);
        GAMENET_TEST_ASSERT(info->nativeError == 0);
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();

    std::function<void()> keepQueueSaturated;
    if (saturate) {
        keepQueueSaturated = [&] {
            if (!peerSawEof) loop.queueInLoop(keepQueueSaturated);
        };
        loop.queueInLoop(keepQueueSaturated);
    }

    auto sendPayloads = [&] {
        for (const auto& payload : payloads) {
            const auto result = connection->trySend(payload);
            if (order == SendShutdownOrder::Sequential) {
                GAMENET_TEST_ASSERT(result == TcpSendResult::Accepted);
            } else {
                GAMENET_TEST_ASSERT(result == TcpSendResult::Accepted ||
                                    result == TcpSendResult::Closed);
            }
            if (result == TcpSendResult::Accepted) acceptedPayload += payload;
        }
    };
    if (order == SendShutdownOrder::Sequential) {
        gamenet::test::runFromNonOwnerThread([&] {
            sendPayloads();
            shutdownResult = connection->tryShutdown();
        });
    } else {
        std::barrier startGate(3);
        std::thread producer([&] {
            startGate.arrive_and_wait();
            sendPayloads();
        });
        std::thread closer([&] {
            startGate.arrive_and_wait();
            shutdownResult = connection->tryShutdown();
        });
        startGate.arrive_and_wait();
        producer.join();
        closer.join();
    }

    // Owner dispatch is still held: these bytes exist only in accepted executor
    // closures, not in Channel write interest or the transport output queue.
    GAMENET_TEST_ASSERT(shutdownResult == PostResult::Accepted);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == acceptedPayload.size());
    GAMENET_TEST_ASSERT(connection->outputMemorySnapshot().pendingBytes == acceptedPayload.size());
    GAMENET_TEST_ASSERT(connection->outputMemorySnapshot().peakPendingBytes == acceptedPayload.size());
    GAMENET_TEST_ASSERT(connection->trySend("late owner") == TcpSendResult::Closed);
    gamenet::test::runFromNonOwnerThread([&] {
        GAMENET_TEST_ASSERT(connection->trySend("late producer") == TcpSendResult::Closed);
        GAMENET_TEST_ASSERT(connection->tryShutdown() == PostResult::Accepted);
    });
    if (saturate) loop.queueInLoop(keepQueueSaturated);

    loop.runEvery(1ms, [&] {
        char buffer[32 * 1024];
        for (;;) {
            const auto count = sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (count > 0) {
                receivedPayload.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) {
                peerSawEof = true;
                GAMENET_TEST_ASSERT(receivedPayload == acceptedPayload);
                GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
                // Repeat after the peer has actually observed half-close.
                GAMENET_TEST_ASSERT(connection->tryShutdown() == PostResult::Accepted);
                gamenet::test::runFromNonOwnerThread([&] {
                    GAMENET_TEST_ASSERT(connection->tryShutdown() == PostResult::Accepted);
                    GAMENET_TEST_ASSERT(connection->trySend("after EOF") == TcpSendResult::Closed);
                });
                connection->forceClose();
                return;
            }
            const auto error = sockets::lastError();
            GAMENET_TEST_ASSERT(sockets::isWouldBlock(error) || sockets::isInterrupted(error));
            return;
        }
    });
    loop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("accepted queued output did not drain before graceful EOF");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(peerSawEof);
    GAMENET_TEST_ASSERT(receivedPayload == acceptedPayload);
    GAMENET_TEST_ASSERT(connectedCallbacks == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbacks == 1);
    GAMENET_TEST_ASSERT(closeCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->disconnected());
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(connection->outputMemorySnapshot().pendingBytes == 0);
    if (saturate) {
        GAMENET_TEST_ASSERT(writeCompleteCallbacks == 0);
        GAMENET_TEST_ASSERT(connection->droppedNotificationCount() == 1);
    } else if (!acceptedPayload.empty()) {
        GAMENET_TEST_ASSERT(writeCompleteCallbacks > 0);
    }
}

void verifyLastReservationRollback(PayloadAllocation action) {
    using namespace gamenet::net;
    using namespace std::chrono_literals;
    EventLoop loop(EventLoopOptions{
        .maxPendingFunctors = 1,
        .reservedPendingFunctors = 1,
        .maxFunctorsPerIteration = 1,
    });
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop, pair, "last-reservation-rollback-resumes-half-close");
    const std::string payload(4096, 'r');
    TcpSendResult sendResult = TcpSendResult::OwnerUnavailable;
    bool allocationFailed = false;
    bool peerSawEof = false;
    unsigned closeCallbacks = 0;
    connection->setCloseCallback([&](const TcpConnectionPtr& conn) {
        ++closeCallbacks;
        GAMENET_TEST_ASSERT(conn->closeInfo()->nativeError == 0);
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();
    loop.queueInLoop([] {});
    loop.queueInLoop([] {});
    payloadAllocationPaused.store(false, std::memory_order_relaxed);
    resumePayloadAllocation.store(false, std::memory_order_relaxed);
    std::thread producer([&] {
        // Debug STL string construction may allocate a small iterator proxy in
        // a noexcept constructor; fault injection must target the payload copy.
        minimumPausedAllocationBytes = payload.size();
        nextPayloadAllocation = action;
        try {
            sendResult = connection->trySend(payload);
        } catch (const std::bad_alloc&) {
            allocationFailed = true;
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!payloadAllocationPaused.load(std::memory_order_acquire)) {
        GAMENET_TEST_ASSERT(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == payload.size());
    GAMENET_TEST_ASSERT(connection->tryShutdown() == PostResult::Accepted);
    GAMENET_TEST_ASSERT(connection->trySend("after seal") == TcpSendResult::Closed);

    // The owner is running, output is sealed and both queues are still full.
    // Resume the producer inside the timer phase so post rejection/throw must
    // roll back the last reservation before any normal task can free capacity.
    loop.runAfter(0ms, [&] {
        GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == payload.size());
        resumePayloadAllocation.store(true, std::memory_order_release);
        producer.join();
        GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    });
    loop.runEvery(1ms, [&] {
        char byte;
        const auto count = sockets::read(pair.peerFd, &byte, 1);
        if (count == 0) {
            peerSawEof = true;
            connection->forceClose();
        } else {
            GAMENET_TEST_ASSERT(count < 0);
            const auto error = sockets::lastError();
            GAMENET_TEST_ASSERT(sockets::isWouldBlock(error) || sockets::isInterrupted(error));
        }
    });
    loop.runAfter(5s, [] {
        GAMENET_TEST_FAIL("last producer rollback stranded graceful half-close");
    });
    loop.loop();
    GAMENET_TEST_ASSERT(peerSawEof);
    GAMENET_TEST_ASSERT(closeCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(connection->outputMemorySnapshot().peakPendingBytes == payload.size());
    if (action == PayloadAllocation::FailAfterPause) {
        GAMENET_TEST_ASSERT(allocationFailed);
    } else {
        GAMENET_TEST_ASSERT(!allocationFailed);
        GAMENET_TEST_ASSERT(sendResult == TcpSendResult::SchedulingQueueFull);
    }
}

void verifyConcurrentOwnerAdmission() {
    using namespace gamenet::net;
    using namespace std::chrono_literals;
    EventLoop loop(EventLoopOptions{.maxFunctorsPerIteration = 1});
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(
        loop, pair, "send-close-race-with-running-owner");
    const std::string payload(1024, 'c');
    std::string received;
    bool peerSawEof = false;
    unsigned closeCallbacks = 0;
    std::atomic<bool> producerFinished{false};
    std::atomic<bool> closerFinished{false};
    TcpSendResult sendResult = TcpSendResult::OwnerUnavailable;
    PostResult shutdownResult = PostResult::OwnerUnavailable;
    connection->setCloseCallback([&](const TcpConnectionPtr& conn) {
        ++closeCallbacks;
        GAMENET_TEST_ASSERT(conn->closeInfo()->nativeError == 0);
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();
    std::barrier startGate(3);
    std::thread producer([&] {
        startGate.arrive_and_wait();
        sendResult = connection->trySend(payload);
        producerFinished.store(true, std::memory_order_release);
    });
    std::thread closer([&] {
        startGate.arrive_and_wait();
        shutdownResult = connection->tryShutdown();
        GAMENET_TEST_ASSERT(connection->trySend("late") == TcpSendResult::Closed);
        closerFinished.store(true, std::memory_order_release);
    });
    loop.runAfter(0ms, [&] {
        // All three execution domains continue from this barrier together.
        startGate.arrive_and_wait();
    });
    loop.runEvery(1ms, [&] {
        char buffer[4096];
        while (!peerSawEof) {
            const auto count = sockets::read(pair.peerFd, buffer, sizeof(buffer));
            if (count > 0) {
                received.append(buffer, static_cast<std::size_t>(count));
            } else if (count == 0) {
                peerSawEof = true;
            } else {
                const auto error = sockets::lastError();
                GAMENET_TEST_ASSERT(sockets::isWouldBlock(error) || sockets::isInterrupted(error));
                break;
            }
        }
        if (peerSawEof && producerFinished.load(std::memory_order_acquire) &&
            closerFinished.load(std::memory_order_acquire)) {
            connection->forceClose();
        }
    });
    loop.runAfter(5s, [] { GAMENET_TEST_FAIL("running owner send-close race did not converge"); });
    loop.loop();
    producer.join();
    closer.join();
    GAMENET_TEST_ASSERT(shutdownResult == PostResult::Accepted);
    GAMENET_TEST_ASSERT(sendResult == TcpSendResult::Accepted || sendResult == TcpSendResult::Closed);
    GAMENET_TEST_ASSERT(received == (sendResult == TcpSendResult::Accepted ? payload : std::string{}));
    GAMENET_TEST_ASSERT(peerSawEof && closeCallbacks == 1);
    GAMENET_TEST_ASSERT(connection->pendingOutputBytes() == 0);
}

}  // namespace

int main() {
    verifyOwnerSendThenForeignShutdown();
    verifyQueuedSendBeforeHalfClose(1, 1, SendShutdownOrder::Sequential);
    verifyQueuedSendBeforeHalfClose(8, 1024, SendShutdownOrder::Sequential);
    verifyQueuedSendBeforeHalfClose(3, 256 * 1024, SendShutdownOrder::Sequential);
    verifyQueuedSendBeforeHalfClose(
        1, 1, SendShutdownOrder::Sequential, NotificationAdmission::Saturated);
    verifyLastReservationRollback(PayloadAllocation::Pause);
    verifyLastReservationRollback(PayloadAllocation::FailAfterPause);
    for (unsigned attempt = 0; attempt < 24; ++attempt) {
        verifyQueuedSendBeforeHalfClose(1, 64, SendShutdownOrder::Concurrent);
        verifyConcurrentOwnerAdmission();
    }
    return 0;
}
