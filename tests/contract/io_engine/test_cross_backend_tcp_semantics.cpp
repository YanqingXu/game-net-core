// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#ifdef GAMENET_TEST_HAS_IO_URING
#include "gamenet/experimental/io_uring/IoUringTcpClient.h"
#include "gamenet/experimental/io_uring/IoUringTcpServer.h"
#endif

#include "support/ClientSocket.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kLowWater = 16U * 1024U;
constexpr std::size_t kHighWater = 64U * 1024U;
constexpr std::size_t kHardLimit = 5U * 1024U * 1024U;
constexpr std::size_t kPressurePayloadBytes = 4U * 1024U * 1024U;
constexpr std::string_view kInboundReply = "x14-inbound-after-pause";
constexpr std::string_view kClientProbe = "x14-client-probe";
constexpr std::string_view kForeignPrefix = "x14-foreign-send";

class OwnedSocket {
public:
    explicit OwnedSocket(
        gamenet::net::SocketFd fd = gamenet::net::kInvalidSocket) noexcept
        : fd_(fd) {}

    ~OwnedSocket() {
        if (gamenet::net::sockets::isValid(fd_)) {
            gamenet::net::sockets::close(fd_);
        }
    }

    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;

    OwnedSocket(OwnedSocket&& other) noexcept
        : fd_(std::exchange(other.fd_, gamenet::net::kInvalidSocket)) {}

    OwnedSocket& operator=(OwnedSocket&& other) noexcept {
        if (this == &other) return *this;
        if (gamenet::net::sockets::isValid(fd_)) {
            gamenet::net::sockets::close(fd_);
        }
        fd_ = std::exchange(other.fd_, gamenet::net::kInvalidSocket);
        return *this;
    }

    gamenet::net::SocketFd get() const noexcept { return fd_; }

private:
    gamenet::net::SocketFd fd_;
};

std::size_t positionOf(
    const std::vector<std::string>& events,
    std::string_view event) {
    const auto found = std::find(events.begin(), events.end(), event);
    GAMENET_TEST_ASSERT(found != events.end());
    return static_cast<std::size_t>(found - events.begin());
}

void waitForSignal(const std::atomic<bool>& signal, std::string_view failure) {
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (!signal.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            GAMENET_TEST_FAIL(failure.data());
        }
        std::this_thread::sleep_for(1ms);
    }
}

void writeAll(gamenet::net::SocketFd fd, std::string_view bytes) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (offset < bytes.size()) {
        const auto written = gamenet::net::sockets::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0) {
            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isInterrupted(error)) continue;
            if (gamenet::net::sockets::isWouldBlock(error)) {
                GAMENET_TEST_ASSERT(
                    std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
                continue;
            }
        }
        GAMENET_TEST_FAIL("X14 peer write failed");
    }
}

std::size_t readUntilEof(gamenet::net::SocketFd fd) {
    std::size_t total = 0;
    char buffer[64U * 1024U];
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (true) {
        const auto count = gamenet::net::sockets::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            total += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return total;
        const int error = gamenet::net::sockets::lastError();
        if (gamenet::net::sockets::isInterrupted(error)) continue;
        if (gamenet::net::sockets::isWouldBlock(error)) {
            GAMENET_TEST_ASSERT(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
            continue;
        }
        GAMENET_TEST_FAIL("X14 peer read failed");
    }
}

std::size_t readExactly(gamenet::net::SocketFd fd, std::size_t expected) {
    std::size_t total = 0;
    char buffer[64U * 1024U];
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (total < expected) {
        const auto count = gamenet::net::sockets::read(
            fd,
            buffer,
            (std::min)(sizeof(buffer), expected - total));
        if (count > 0) {
            total += static_cast<std::size_t>(count);
            continue;
        }
        GAMENET_TEST_ASSERT(count != 0);
        const int error = gamenet::net::sockets::lastError();
        if (gamenet::net::sockets::isInterrupted(error)) continue;
        if (gamenet::net::sockets::isWouldBlock(error)) {
            GAMENET_TEST_ASSERT(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
            continue;
        }
        GAMENET_TEST_FAIL("X14 peer exact read failed");
    }
    return total;
}

struct ForeignAdmission {
    gamenet::net::TcpSendResult overload{
        gamenet::net::TcpSendResult::OwnerUnavailable};
    gamenet::net::TcpSendResult send{
        gamenet::net::TcpSendResult::OwnerUnavailable};
    gamenet::net::PostResult shutdown{gamenet::net::PostResult::OwnerUnavailable};
    gamenet::net::PostResult repeatedShutdown{
        gamenet::net::PostResult::OwnerUnavailable};
    gamenet::net::TcpSendResult lateSend{
        gamenet::net::TcpSendResult::OwnerUnavailable};
};

struct PeerOutcome {
    ForeignAdmission admission{};
    std::size_t drainedBytes{};
    bool sawEof{};
    bool sentInbound{};
    bool halfClosed{};
};

struct ServerSemanticTrace {
    bool callbacksOnOwner{};
    bool overloadRejected{};
    bool foreignSendAccepted{};
    bool foreignShutdownAccepted{};
    bool repeatedShutdownAccepted{};
    bool postShutdownSendRejected{};
    bool highWaterObserved{};
    bool readPaused{};
    bool inboundSuppressedWhilePaused{};
    bool readResumed{};
    bool writeCompleteObserved{};
    bool peerSawCompletePayloadBeforeEof{};
    bool peerHalfClosed{};
    bool closeReasonGraceful{};
    bool pendingOutputDrained{};
    bool finalDrainReady{};
    bool orderingValid{};
    std::string inbound;

    bool operator==(const ServerSemanticTrace&) const = default;
};

struct ClientSemanticTrace {
    bool callbacksOnOwner{};
    bool sendAccepted{};
    bool shutdownAccepted{};
    bool postShutdownSendRejected{};
    bool echoObserved{};
    bool closeReasonGraceful{};
    bool finalDrainReady{};
    bool orderingValid{};

    bool operator==(const ClientSemanticTrace&) const = default;
};

template <typename ConnectionFuture, typename AdmissionFunction>
std::jthread startPressurePeer(
    gamenet::net::InetAddress address,
    ConnectionFuture connectionFuture,
    AdmissionFunction admit,
    const std::string* oversized,
    const std::string* payload,
    const std::atomic<bool>* startDraining,
    PeerOutcome* outcome) {
    return std::jthread(
        [address,
         connectionFuture = std::move(connectionFuture),
         admit = std::move(admit),
         oversized,
         payload,
         startDraining,
         outcome] mutable {
            OwnedSocket peer(gamenet::test::connectTestClientWithReceiveBuffer(
                address, 4096));
            auto connection = connectionFuture.get();
            outcome->admission = admit(connection, *oversized, *payload);
            writeAll(peer.get(), kInboundReply);
            outcome->sentInbound = true;
            waitForSignal(*startDraining, "X14 peer timed out waiting to drain");
            outcome->drainedBytes = readUntilEof(peer.get());
            outcome->sawEof = true;
            gamenet::net::sockets::shutdownWrite(peer.get());
            outcome->halfClosed = true;
        });
}

void verifyServerTrace(const ServerSemanticTrace& trace) {
    GAMENET_TEST_ASSERT(trace.callbacksOnOwner);
    GAMENET_TEST_ASSERT(trace.overloadRejected);
    GAMENET_TEST_ASSERT(trace.foreignSendAccepted);
    GAMENET_TEST_ASSERT(trace.foreignShutdownAccepted);
    GAMENET_TEST_ASSERT(trace.repeatedShutdownAccepted);
    GAMENET_TEST_ASSERT(trace.postShutdownSendRejected);
    GAMENET_TEST_ASSERT(trace.highWaterObserved);
    GAMENET_TEST_ASSERT(trace.readPaused);
    GAMENET_TEST_ASSERT(trace.inboundSuppressedWhilePaused);
    GAMENET_TEST_ASSERT(trace.readResumed);
    GAMENET_TEST_ASSERT(trace.writeCompleteObserved);
    GAMENET_TEST_ASSERT(trace.peerSawCompletePayloadBeforeEof);
    GAMENET_TEST_ASSERT(trace.peerHalfClosed);
    GAMENET_TEST_ASSERT(trace.closeReasonGraceful);
    GAMENET_TEST_ASSERT(trace.pendingOutputDrained);
    GAMENET_TEST_ASSERT(trace.finalDrainReady);
    GAMENET_TEST_ASSERT(trace.orderingValid);
    GAMENET_TEST_ASSERT(trace.inbound == kInboundReply);
}

void verifyClientTrace(const ClientSemanticTrace& trace) {
    GAMENET_TEST_ASSERT(trace.callbacksOnOwner);
    GAMENET_TEST_ASSERT(trace.sendAccepted);
    GAMENET_TEST_ASSERT(trace.shutdownAccepted);
    GAMENET_TEST_ASSERT(trace.postShutdownSendRejected);
    GAMENET_TEST_ASSERT(trace.echoObserved);
    GAMENET_TEST_ASSERT(trace.closeReasonGraceful);
    GAMENET_TEST_ASSERT(trace.finalDrainReady);
    GAMENET_TEST_ASSERT(trace.orderingValid);
}

ServerSemanticTrace runProductionServerSemantics() {
#ifdef _WIN32
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair(
        gamenet::test::SocketPairMode::SmallSendBuffer);
    auto connection = gamenet::test::makeTcpConnection(
        loop, pair, "x14-production-iocp-connection");
    connection->setBackpressureOptions({
        .lowWaterMarkBytes = kLowWater,
        .highWaterMarkBytes = kHighWater,
        .hardLimitBytes = kHardLimit,
        .maxInputBufferBytes = 64U * 1024U,
    });

    const std::string oversized(kHardLimit + 1U, 'o');
    const std::string payload(kPressurePayloadBytes, 'p');
    std::promise<gamenet::net::TcpConnectionPtr> connectionPromise;
    auto connectionFuture = connectionPromise.get_future().share();
    std::atomic<bool> highWaterSeen{false};
    std::atomic<bool> startDraining{false};
    gamenet::net::TcpSendResult pressureSendResult{
        gamenet::net::TcpSendResult::OwnerUnavailable};
    PeerOutcome peerOutcome;
    ServerSemanticTrace trace;
    std::vector<std::string> events;
    std::size_t messageCallbacks = 0;

    connection->setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& current) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            trace.callbacksOnOwner = true;
            if (current->connected()) {
                events.emplace_back("connected");
                connectionPromise.set_value(current);
            } else {
                events.emplace_back("disconnected");
            }
        });
    connection->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& current,
            std::size_t bytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(bytes >= kHighWater);
            GAMENET_TEST_ASSERT(current->readingPausedByBackpressure());
            events.emplace_back("high_water");
            trace.highWaterObserved = true;
            trace.readPaused = true;
            writeAll(pair.peerFd, kInboundReply);
            peerOutcome.sentInbound = true;
            GAMENET_TEST_ASSERT(messageCallbacks == 0);
            trace.inboundSuppressedWhilePaused = true;
            highWaterSeen.store(true, std::memory_order_release);
            startDraining.store(true, std::memory_order_release);
        },
        1024U);
    connection->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& current,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                !current->readingPausedByBackpressure());
            events.emplace_back("message");
            trace.readResumed = true;
            trace.inbound += input->retrieveAllAsString();
            ++messageCallbacks;
        });
    connection->setWriteCompleteCallback(
        [&](const gamenet::net::TcpConnectionPtr&) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.emplace_back("write_complete");
            trace.writeCompleteObserved = true;
        });
    connection->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr& current,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(current->socketClosed());
            GAMENET_TEST_ASSERT(
                current->closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            events.emplace_back("close_info");
            trace.closeReasonGraceful =
                info.reason ==
                    gamenet::net::TcpConnectionCloseReason::GracefulShutdown &&
                info.nativeError == 0;
            trace.pendingOutputDrained = current->pendingOutputBytes() == 0;
        });
    connection->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& current) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            current->connectDestroyed();
            events.emplace_back("final_drain");
            trace.finalDrainReady =
                current->disconnected() && current->socketClosed() &&
                current->pendingOutputBytes() == 0;
            loop.quit();
        });

    std::jthread peer([&] {
        const auto current = connectionFuture.get();
        waitForSignal(
            highWaterSeen,
            "X14 IOCP peer timed out waiting for high water");
        waitForSignal(
            startDraining,
            "X14 IOCP peer timed out waiting to drain");
        peerOutcome.admission.send = current->trySend(kForeignPrefix);
        peerOutcome.drainedBytes = readExactly(
            pair.peerFd,
            payload.size() + kForeignPrefix.size());
        peerOutcome.admission.shutdown = current->tryShutdown();
        peerOutcome.admission.repeatedShutdown = current->tryShutdown();
        peerOutcome.admission.lateSend = current->trySend("late");
        GAMENET_TEST_ASSERT(readUntilEof(pair.peerFd) == 0);
        peerOutcome.sawEof = true;
        gamenet::net::sockets::shutdownWrite(pair.peerFd);
        peerOutcome.halfClosed = true;
    });

    loop.runAfter(0ms, [&] {
        connection->connectEstablished();
        peerOutcome.admission.overload = connection->trySend(oversized);
        pressureSendResult = connection->trySend(payload);
    });
    loop.runAfter(12s, [] {
        GAMENET_TEST_FAIL("IOE-X14 production IOCP semantics timed out");
    });
    loop.loop();
    peer.join();

    GAMENET_TEST_ASSERT(
        pressureSendResult == gamenet::net::TcpSendResult::Accepted);
    trace.overloadRejected =
        peerOutcome.admission.overload == gamenet::net::TcpSendResult::Overloaded;
    trace.foreignSendAccepted =
        peerOutcome.admission.send == gamenet::net::TcpSendResult::Accepted;
    trace.foreignShutdownAccepted =
        peerOutcome.admission.shutdown == gamenet::net::PostResult::Accepted;
    trace.repeatedShutdownAccepted =
        peerOutcome.admission.repeatedShutdown ==
        gamenet::net::PostResult::Accepted;
    trace.postShutdownSendRejected =
        peerOutcome.admission.lateSend == gamenet::net::TcpSendResult::Closed;
    trace.peerSawCompletePayloadBeforeEof =
        peerOutcome.sawEof &&
        peerOutcome.drainedBytes == payload.size() + kForeignPrefix.size();
    trace.peerHalfClosed = peerOutcome.halfClosed;
    trace.orderingValid =
        positionOf(events, "connected") < positionOf(events, "high_water") &&
        positionOf(events, "high_water") < positionOf(events, "message") &&
        positionOf(events, "write_complete") < positionOf(events, "close_info") &&
        positionOf(events, "write_complete") <
            positionOf(events, "disconnected") &&
        positionOf(events, "close_info") < positionOf(events, "final_drain") &&
        positionOf(events, "disconnected") < positionOf(events, "final_drain");
    verifyServerTrace(trace);
    return trace;
#else
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop, gamenet::net::InetAddress(0, true), "x14-production-server");
    server.setConnectionBackpressureOptions({
        .lowWaterMarkBytes = kLowWater,
        .highWaterMarkBytes = kHighWater,
        .hardLimitBytes = kHardLimit,
        .maxInputBufferBytes = 64U * 1024U,
    });

    const std::string oversized(kHardLimit + 1U, 'o');
    const std::string payload(kPressurePayloadBytes, 'p');
    std::promise<gamenet::net::TcpConnectionPtr> connectionPromise;
    auto connectionFuture = connectionPromise.get_future().share();
    std::atomic<bool> highWaterSeen{false};
    std::atomic<bool> startDraining{false};
    std::atomic<bool> admissionFinished{false};
    PeerOutcome peerOutcome;
    ServerSemanticTrace trace;
    std::vector<std::string> events;
    std::size_t messageCallbacks = 0;
    bool connectionPublished = false;
    gamenet::net::TcpServerStopFuture stopFuture;

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            trace.callbacksOnOwner =
                trace.callbacksOnOwner || loop.isInLoopThread();
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                events.emplace_back("connected");
                connection->setSendBufferSize(4096);
                if (!connectionPublished) {
                    connectionPublished = true;
                    connectionPromise.set_value(connection);
                }
                return;
            }
            events.emplace_back("disconnected");
            if (!stopFuture.valid()) {
                stopFuture = server.stopGracefully(
                    gamenet::net::TcpServerStopOptions{.drainTimeout = 2s});
            }
        });
    server.setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            std::size_t bytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                admissionFinished.load(std::memory_order_acquire));
            GAMENET_TEST_ASSERT(bytes >= kHighWater);
            GAMENET_TEST_ASSERT(connection->readingPausedByBackpressure());
            events.emplace_back("high_water");
            trace.highWaterObserved = true;
            trace.readPaused = true;
            GAMENET_TEST_ASSERT(messageCallbacks == 0);
            trace.inboundSuppressedWhilePaused = true;
            highWaterSeen.store(true, std::memory_order_release);
            startDraining.store(true, std::memory_order_release);
        },
        kHighWater);
    server.setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                !connection->readingPausedByBackpressure());
            events.emplace_back("message");
            trace.readResumed = true;
            trace.inbound += input->retrieveAllAsString();
            ++messageCallbacks;
        });
    server.setWriteCompleteCallback(
        [&](const gamenet::net::TcpConnectionPtr&) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.emplace_back("write_complete");
            trace.writeCompleteObserved = true;
        });
    server.setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(connection->socketClosed());
            GAMENET_TEST_ASSERT(
                connection->closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            events.emplace_back("close_info");
            trace.closeReasonGraceful =
                info.reason ==
                    gamenet::net::TcpConnectionCloseReason::GracefulShutdown &&
                info.nativeError == 0;
            trace.pendingOutputDrained = connection->pendingOutputBytes() == 0;
        });

    server.start();
    auto peer = startPressurePeer(
        server.listenAddress(),
        connectionFuture,
        [&](const gamenet::net::TcpConnectionPtr& connection,
           const std::string& oversizedBytes,
           const std::string& acceptedBytes) {
            ForeignAdmission result;
            result.overload = connection->trySend(oversizedBytes);
            result.send = connection->trySend(acceptedBytes);
            admissionFinished.store(true, std::memory_order_release);
            waitForSignal(
                highWaterSeen,
                "X14 production peer timed out waiting for high water");
            result.shutdown = connection->tryShutdown();
            result.repeatedShutdown = connection->tryShutdown();
            result.lateSend = connection->trySend("late");
            return result;
        },
        &oversized,
        &payload,
        &startDraining,
        &peerOutcome);

    const auto progress = loop.runEvery(1ms, [&] {
        if (!stopFuture.valid() ||
            stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        const auto result = stopFuture.get();
        GAMENET_TEST_ASSERT(
            result.outcome == gamenet::net::TcpServerStopOutcome::Drained);
        GAMENET_TEST_ASSERT(server.connectionCount() == 0);
        events.emplace_back("final_drain");
        trace.finalDrainReady = true;
        loop.quit();
    });
    loop.runAfter(12s, [] {
        GAMENET_TEST_FAIL("IOE-X14 production Server semantics timed out");
    });
    loop.loop();
    loop.cancel(progress);
    peer.join();

    trace.callbacksOnOwner = trace.callbacksOnOwner && connectionPublished;
    trace.overloadRejected =
        peerOutcome.admission.overload == gamenet::net::TcpSendResult::Overloaded;
    trace.foreignSendAccepted =
        peerOutcome.admission.send == gamenet::net::TcpSendResult::Accepted;
    trace.foreignShutdownAccepted =
        peerOutcome.admission.shutdown == gamenet::net::PostResult::Accepted;
    trace.repeatedShutdownAccepted =
        peerOutcome.admission.repeatedShutdown ==
        gamenet::net::PostResult::Accepted;
    trace.postShutdownSendRejected =
        peerOutcome.admission.lateSend == gamenet::net::TcpSendResult::Closed;
    trace.peerSawCompletePayloadBeforeEof =
        peerOutcome.sawEof && peerOutcome.drainedBytes == payload.size();
    trace.peerHalfClosed = peerOutcome.halfClosed;
    trace.orderingValid =
        positionOf(events, "connected") < positionOf(events, "high_water") &&
        positionOf(events, "high_water") < positionOf(events, "message") &&
        positionOf(events, "write_complete") < positionOf(events, "close_info") &&
        positionOf(events, "write_complete") <
            positionOf(events, "disconnected") &&
        positionOf(events, "close_info") < positionOf(events, "final_drain") &&
        positionOf(events, "disconnected") < positionOf(events, "final_drain");
    verifyServerTrace(trace);
    return trace;
#endif
}

ClientSemanticTrace runProductionClientSemantics() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop, gamenet::net::InetAddress(0, true), "x14-production-client-peer");
    std::unique_ptr<gamenet::net::TcpClient> client;
    gamenet::net::TcpServerStopFuture serverStop;
    gamenet::net::TcpConnectionPtr serverConnection;
    ClientSemanticTrace trace;
    std::vector<std::string> events;

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                serverConnection = connection;
            } else if (serverConnection == connection) {
                serverConnection.reset();
            }
        });
    server.setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            const auto payload = input->retrieveAllAsString();
            GAMENET_TEST_ASSERT(payload == kClientProbe);
            GAMENET_TEST_ASSERT(
                connection->trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    server.start();

    client = std::make_unique<gamenet::net::TcpClient>(
        &loop, server.listenAddress(), "x14-production-client");
    client->setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            trace.callbacksOnOwner = true;
            if (connection->connected()) {
                events.emplace_back("connected");
                trace.sendAccepted =
                    connection->trySend(kClientProbe) ==
                    gamenet::net::TcpSendResult::Accepted;
                return;
            }
            events.emplace_back("disconnected");
            client->stop();
            if (!serverStop.valid()) {
                serverStop = server.stopGracefully(
                    gamenet::net::TcpServerStopOptions{.drainTimeout = 2s});
            }
        });
    client->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(input->retrieveAllAsString() == kClientProbe);
            events.emplace_back("message");
            trace.echoObserved = true;
            trace.shutdownAccepted =
                connection->tryShutdown() == gamenet::net::PostResult::Accepted;
            trace.postShutdownSendRejected =
                connection->trySend("late") ==
                gamenet::net::TcpSendResult::Closed;
            GAMENET_TEST_ASSERT(serverConnection);
            GAMENET_TEST_ASSERT(
                serverConnection->tryShutdown() ==
                gamenet::net::PostResult::Accepted);
        });
    client->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.emplace_back("close_info");
            trace.closeReasonGraceful =
                info.reason ==
                    gamenet::net::TcpConnectionCloseReason::GracefulShutdown &&
                info.nativeError == 0;
        });

    GAMENET_TEST_ASSERT(
        client->tryConnect() == gamenet::net::PostResult::Accepted);
    const auto progress = loop.runEvery(1ms, [&] {
        if (!serverStop.valid() ||
            serverStop.wait_for(0ms) != std::future_status::ready ||
            client->connection()) {
            return;
        }
        const auto result = serverStop.get();
        GAMENET_TEST_ASSERT(
            result.outcome == gamenet::net::TcpServerStopOutcome::Drained);
        events.emplace_back("final_drain");
        trace.finalDrainReady = server.connectionCount() == 0;
        loop.quit();
    });
    loop.runAfter(8s, [] {
        GAMENET_TEST_FAIL("IOE-X14 production Client semantics timed out");
    });
    loop.loop();
    loop.cancel(progress);

    trace.orderingValid =
        positionOf(events, "connected") < positionOf(events, "message") &&
        positionOf(events, "message") < positionOf(events, "close_info") &&
        positionOf(events, "message") < positionOf(events, "disconnected") &&
        positionOf(events, "close_info") < positionOf(events, "final_drain") &&
        positionOf(events, "disconnected") < positionOf(events, "final_drain");
    verifyClientTrace(trace);
    return trace;
}

#ifdef GAMENET_TEST_HAS_IO_URING

namespace uring = gamenet::experimental::io_uring;

uring::IoUringTcpConnectionHubOptions ioUringHubOptions(
    bool withListener = false) {
    auto options = uring::IoUringTcpConnectionHubOptions{
        .pump = {
            .engine = {
                .entries = 256,
                .maxOperations = 16,
                .maxCompletionsPerWait = 64,
                .maxBytesPerOperation = 16U * 1024U,
                .maxOwnedBytes = 128U * 1024U,
            },
            .maxNoticesPerTurn = 8,
        },
        .maxConnections = 2,
        .maxTotalPendingSendBytes = 2U * kHardLimit,
        .maxReceiveBytes = 4U * 1024U,
        .maxSendBytesPerOperation = 16U * 1024U,
        .maxPendingSendBytesPerConnection = kHardLimit,
        .maxPendingSendSegmentsPerConnection = 8,
        .maxPendingAccepts = 0,
    };
    options.maxPendingAccepts = withListener ? 2U : 1U;
    return options;
}

uring::IoUringTcpConnectionAdapterOptions ioUringConnectionOptions() {
    return {
        .lowWaterMarkBytes = kLowWater,
        .highWaterMarkBytes = kHighWater,
        .hardLimitBytes = kHardLimit,
        .maxPendingCommands = 32,
        .maxCommandsPerTurn = 8,
    };
}

void verifyExperimentalServerResidue(
    const uring::IoUringTcpServerStopSummary& summary) {
    GAMENET_TEST_ASSERT(summary.listenerStoppedBeforeHub);
    GAMENET_TEST_ASSERT(summary.allAdaptersRetired);
    GAMENET_TEST_ASSERT(summary.server.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.server.provisionalConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.hub.listener.has_value());
    GAMENET_TEST_ASSERT(summary.hub.listener->socketClosed);
    GAMENET_TEST_ASSERT(summary.hub.listener->acceptsRetired);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.invariantFailures == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.pendingSubmissions == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.ownedBytes == 0);
}

void verifyExperimentalClientResidue(
    const uring::IoUringTcpClientStopSummary& summary) {
    GAMENET_TEST_ASSERT(summary.allTimersRetired);
    GAMENET_TEST_ASSERT(summary.allAttemptsRetired);
    GAMENET_TEST_ASSERT(summary.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.ownerDestroyedHub);
    GAMENET_TEST_ASSERT(summary.client.activeAttempts == 0);
    GAMENET_TEST_ASSERT(summary.client.activeConnections == 0);
    GAMENET_TEST_ASSERT(!summary.client.retryTimerPending);
    GAMENET_TEST_ASSERT(!summary.client.timeoutTimerPending);
    GAMENET_TEST_ASSERT(summary.client.hubDestroyed);
    GAMENET_TEST_ASSERT(summary.hub.allConnectionsStopped);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(summary.hub.hub.invariantFailures == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.pendingSubmissions == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(summary.hub.pump.engine.ownedBytes == 0);
}

ServerSemanticTrace runIoUringServerSemantics() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> stopped;
    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        uring::IoUringTcpServerOptions{
            .hub = ioUringHubOptions(true),
            .connection = ioUringConnectionOptions(),
            .reuseAddress = true,
            .reusePort = false,
        },
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            stopped = summary;
            loop.quit();
        });

    const std::string oversized(kHardLimit + 1U, 'o');
    const std::string payload(kPressurePayloadBytes, 'p');
    std::promise<uring::IoUringTcpConnectionAdapter*> connectionPromise;
    auto connectionFuture = connectionPromise.get_future().share();
    std::atomic<bool> highWaterSeen{false};
    std::atomic<bool> startDraining{false};
    std::atomic<bool> admissionFinished{false};
    PeerOutcome peerOutcome;
    ServerSemanticTrace trace;
    std::vector<std::string> events;
    std::size_t messageCallbacks = 0;
    bool connectionPublished = false;
    std::shared_future<uring::IoUringTcpConnectionAdapterStopSummary>
        connectionStop;

    server.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            trace.callbacksOnOwner = true;
            if (connection.connected()) {
                events.emplace_back("connected");
                if (!connectionPublished) {
                    connectionPublished = true;
                    connectionStop = connection.stopFuture();
                    connectionPromise.set_value(&connection);
                }
                return;
            }
            events.emplace_back("disconnected");
            (void)server.stopGracefully();
        });
    server.setHighWaterMarkCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t bytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                admissionFinished.load(std::memory_order_acquire));
            GAMENET_TEST_ASSERT(bytes >= kHighWater);
            GAMENET_TEST_ASSERT(connection.readingPausedByBackpressure());
            events.emplace_back("high_water");
            trace.highWaterObserved = true;
            trace.readPaused = true;
            GAMENET_TEST_ASSERT(messageCallbacks == 0);
            trace.inboundSuppressedWhilePaused = true;
            highWaterSeen.store(true, std::memory_order_release);
            startDraining.store(true, std::memory_order_release);
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(!connection.readingPausedByBackpressure());
            events.emplace_back("message");
            trace.readResumed = true;
            trace.inbound.append(input);
            ++messageCallbacks;
        });
    server.setWriteCompleteCallback(
        [&](uring::IoUringTcpConnectionAdapter&) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.emplace_back("write_complete");
            trace.writeCompleteObserved = true;
        });
    server.setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(
                connectionStop.wait_for(0s) == std::future_status::ready);
            GAMENET_TEST_ASSERT(
                connection.closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            events.emplace_back("close_info");
            trace.closeReasonGraceful =
                info.reason ==
                    gamenet::net::TcpConnectionCloseReason::GracefulShutdown &&
                info.nativeError == 0;
            trace.pendingOutputDrained = connection.pendingOutputBytes() == 0;
        });

    const auto start = server.start();
    GAMENET_TEST_ASSERT(
        start.result == uring::IoUringTcpServerStartResult::Accepted);
    auto peer = startPressurePeer(
        start.listenAddress,
        connectionFuture,
        [&](uring::IoUringTcpConnectionAdapter* connection,
           const std::string& oversizedBytes,
           const std::string& acceptedBytes) {
            ForeignAdmission result;
            result.overload = connection->trySend(oversizedBytes);
            result.send = connection->trySend(acceptedBytes);
            admissionFinished.store(true, std::memory_order_release);
            waitForSignal(
                highWaterSeen,
                "X14 io_uring peer timed out waiting for high water");
            result.shutdown = connection->tryShutdown();
            result.repeatedShutdown = connection->tryShutdown();
            result.lateSend = connection->trySend("late");
            return result;
        },
        &oversized,
        &payload,
        &startDraining,
        &peerOutcome);
    loop.runAfter(12s, [] {
        GAMENET_TEST_FAIL("IOE-X14 io_uring Server semantics timed out");
    });
    loop.loop();
    peer.join();

    GAMENET_TEST_ASSERT(stopped.has_value());
    verifyExperimentalServerResidue(*stopped);
    events.emplace_back("final_drain");
    trace.finalDrainReady = true;
    trace.overloadRejected =
        peerOutcome.admission.overload == gamenet::net::TcpSendResult::Overloaded;
    trace.foreignSendAccepted =
        peerOutcome.admission.send == gamenet::net::TcpSendResult::Accepted;
    trace.foreignShutdownAccepted =
        peerOutcome.admission.shutdown == gamenet::net::PostResult::Accepted;
    trace.repeatedShutdownAccepted =
        peerOutcome.admission.repeatedShutdown ==
        gamenet::net::PostResult::Accepted;
    trace.postShutdownSendRejected =
        peerOutcome.admission.lateSend == gamenet::net::TcpSendResult::Closed;
    trace.peerSawCompletePayloadBeforeEof =
        peerOutcome.sawEof && peerOutcome.drainedBytes == payload.size();
    trace.peerHalfClosed = peerOutcome.halfClosed;
    trace.orderingValid =
        positionOf(events, "connected") < positionOf(events, "high_water") &&
        positionOf(events, "high_water") < positionOf(events, "message") &&
        positionOf(events, "write_complete") < positionOf(events, "close_info") &&
        positionOf(events, "write_complete") <
            positionOf(events, "disconnected") &&
        positionOf(events, "close_info") < positionOf(events, "final_drain") &&
        positionOf(events, "disconnected") < positionOf(events, "final_drain");
    verifyServerTrace(trace);
    return trace;
}

ClientSemanticTrace runIoUringClientSemantics() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpServerStopSummary> serverStopped;
    std::optional<uring::IoUringTcpClientStopSummary> clientStopped;
    ClientSemanticTrace trace;
    std::vector<std::string> events;

    uring::IoUringTcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        uring::IoUringTcpServerOptions{
            .hub = ioUringHubOptions(true),
            .connection = ioUringConnectionOptions(),
            .reuseAddress = true,
            .reusePort = false,
        },
        [&](const uring::IoUringTcpServerStopSummary& summary) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            serverStopped = summary;
            if (clientStopped.has_value()) loop.quit();
        });
    server.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(payload == kClientProbe);
            GAMENET_TEST_ASSERT(
                connection.trySend(payload) ==
                gamenet::net::TcpSendResult::Accepted);
        });
    const auto serverStart = server.start();
    GAMENET_TEST_ASSERT(
        serverStart.result == uring::IoUringTcpServerStartResult::Accepted);

    uring::IoUringTcpClient client(
        &loop,
        serverStart.listenAddress,
        uring::IoUringTcpClientOptions{
            .hub = ioUringHubOptions(false),
            .connection = ioUringConnectionOptions(),
            .initialRetryDelay = 10ms,
            .maximumRetryDelay = 100ms,
            .connectTimeout = 2s,
            .retryEnabled = false,
        },
        [&](const uring::IoUringTcpClientStopSummary& summary) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            clientStopped = summary;
            if (serverStopped.has_value()) loop.quit();
        });
    client.setConnectionCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            trace.callbacksOnOwner = true;
            if (connection.connected()) {
                events.emplace_back("connected");
                trace.sendAccepted =
                    connection.trySend(kClientProbe) ==
                    gamenet::net::TcpSendResult::Accepted;
                return;
            }
            events.emplace_back("disconnected");
            (void)client.stop();
            (void)server.stopGracefully();
        });
    client.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(payload == kClientProbe);
            events.emplace_back("message");
            trace.echoObserved = true;
            trace.shutdownAccepted =
                connection.tryShutdown() == gamenet::net::PostResult::Accepted;
            trace.postShutdownSendRejected =
                connection.trySend("late") ==
                gamenet::net::TcpSendResult::Closed;
        });
    client.setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter&,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            events.emplace_back("close_info");
            trace.closeReasonGraceful =
                info.reason ==
                    gamenet::net::TcpConnectionCloseReason::GracefulShutdown &&
                info.nativeError == 0;
        });

    GAMENET_TEST_ASSERT(
        client.connect().result ==
        uring::IoUringTcpClientConnectResult::Accepted);
    loop.runAfter(8s, [] {
        GAMENET_TEST_FAIL("IOE-X14 io_uring Client semantics timed out");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(serverStopped.has_value());
    GAMENET_TEST_ASSERT(clientStopped.has_value());
    verifyExperimentalServerResidue(*serverStopped);
    verifyExperimentalClientResidue(*clientStopped);
    events.emplace_back("final_drain");
    trace.finalDrainReady = true;
    trace.orderingValid =
        positionOf(events, "connected") < positionOf(events, "message") &&
        positionOf(events, "message") < positionOf(events, "close_info") &&
        positionOf(events, "message") < positionOf(events, "disconnected") &&
        positionOf(events, "close_info") < positionOf(events, "final_drain") &&
        positionOf(events, "disconnected") < positionOf(events, "final_drain");
    verifyClientTrace(trace);
    return trace;
}

#endif

}  // namespace

int main() {
    const auto productionServer = runProductionServerSemantics();
    const auto productionClient = runProductionClientSemantics();

#ifdef GAMENET_TEST_HAS_IO_URING
    const auto ioUringServer = runIoUringServerSemantics();
    const auto ioUringClient = runIoUringClientSemantics();
    GAMENET_TEST_ASSERT(ioUringServer == productionServer);
    GAMENET_TEST_ASSERT(ioUringClient == productionClient);
#endif

    return 0;
}
