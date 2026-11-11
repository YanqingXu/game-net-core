// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerViewReceiver.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpServer.h"
#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace gamenet::experimental::hp1;
using gamenet::net::SocketFd;

FrameReceiverOptions receiverOptions(std::size_t frames = 2) {
    return {
        .framing = {.maxPayloadBytes = 16, .maxBufferedBytes = 128,
                    .maxFramesPerPush = frames, .maxFrameBytesPerPush = 40},
        .maxHandlerWallTime = 1s,
    };
}

std::string encode(std::string_view payload) {
    return *gamenet::protocol::PacketFramer(receiverOptions().framing).encode(payload);
}

bool writeBytes(SocketFd socket, std::string_view bytes) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t offset = 0;
    while (offset < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::write(socket, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count < 0) {
            const auto error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInProgress(error) &&
                !gamenet::net::sockets::isInterrupted(error)) return false;
            std::this_thread::sleep_for(1ms);
        }
    }
    return offset == bytes.size();
}

bool readBytes(SocketFd socket, const std::string& expected, bool requireEof) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::string received;
    std::array<char, 256> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(socket, buffer.data(), buffer.size());
        if (count > 0) {
            received.append(buffer.data(), static_cast<std::size_t>(count));
            if (!requireEof && received == expected) return true;
        } else if (count == 0) {
            return received == expected;
        } else {
            const auto error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInterrupted(error)) return received == expected && requireEof;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

template <typename Client>
FrameReceiverSnapshot runScenario(
    FrameReceiverOptions options, PacketFramerViewReceiver::Handler handler,
    Client clientOperation, std::size_t batchBytes = 0,
    gamenet::net::EventLoopOptions loopOptions = {},
    gamenet::net::EventLoop** loopObserver = nullptr,
    PacketFramerViewReceiver** receiverObserver = nullptr,
    gamenet::net::TcpServer** serverObserver = nullptr) {
    gamenet::net::EventLoop loop(loopOptions);
    if (loopObserver) *loopObserver = &loop;
    std::shared_ptr<PacketFramerViewReceiver> receiver;
    std::size_t coreCallbackExceptions = 0;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "hp1_receiver_contract");
    if (serverObserver) *serverObserver = &server;
    server.setThreadNum(0);
    server.setConnectionCallback([&](const auto& connection) {
        if (connection->connected()) {
            connection->setCallbackExceptionHandler([&](const auto&, const auto&) {
                ++coreCallbackExceptions;
            });
            receiver = PacketFramerViewReceiver::create(connection, handler, options);
            if (receiverObserver) *receiverObserver = receiver.get();
            connection->setContext(receiver);
        } else {
            receiver->stop();
            connection->setContext(std::any{});
        }
    });
    std::atomic<bool> partialSeen{false};
    bool dispatched = false;
    server.setMessageCallback([&](const auto&, gamenet::net::Buffer* input) {
        if (!dispatched && input->readableBytes() < batchBytes) return;
        dispatched = true;
        receiver->onMessage(*input);
        if (input->readableBytes() == 2) partialSeen.store(true, std::memory_order_release);
    });
    server.start();
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::thread client([&] {
        const auto socket = gamenet::test::connectTestClient(server.listenAddress());
        success.store(clientOperation(socket, partialSeen));
        gamenet::test::closeTestSocket(socket);
        done.store(true, std::memory_order_release);
    });
    gamenet::net::TcpServerStopFuture stopped;
    loop.runEvery(1ms, [&] {
        if (!done.load(std::memory_order_acquire)) return;
        if (!stopped.valid()) {
            if (receiver) receiver->stop();
            stopped = server.stopGracefully({.drainTimeout = 0ms});
        }
        if (stopped.wait_for(0s) == std::future_status::ready) loop.quit();
    });
    gamenet::test::runLoopWithTimeout(loop, 5s, "HP1 TCP receiver did not settle");
    client.join();
    GAMENET_TEST_ASSERT(success.load());
    GAMENET_TEST_ASSERT(receiver != nullptr);
    GAMENET_TEST_ASSERT(coreCallbackExceptions == 0);
    const auto result = receiver->snapshot();
    GAMENET_TEST_ASSERT(result.settled());
    return result;
}

void realInputRetainsPartialFramesAndEchoesInOrder() {
    const auto owner = std::this_thread::get_id();
    std::vector<OwnedPacket> retained;
    std::string wire;
    for (const auto payload : {"one", "two", "three", "four", "five"}) wire += encode(payload);
    const auto result = runScenario(receiverOptions(), [&](const auto& connection, PacketView view) {
        GAMENET_TEST_ASSERT(std::this_thread::get_id() == owner);
        retained.push_back(view.retain());
        GAMENET_TEST_ASSERT(connection->trySend(encode(view.asStringView())) == gamenet::net::TcpSendResult::Accepted);
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto& partialSeen) {
        if (!writeBytes(socket, std::string_view(wire).substr(0, 2))) return false;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!partialSeen.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return writeBytes(socket, std::string_view(wire).substr(2)) && readBytes(socket, wire, false);
    });
    GAMENET_TEST_ASSERT(result.handlerCalls == 5);
    GAMENET_TEST_ASSERT(result.maxHandlersPerDispatch <= 2);
    GAMENET_TEST_ASSERT(result.consumedBytes == wire.size());
    GAMENET_TEST_ASSERT(result.discardedBytes == 0);
    GAMENET_TEST_ASSERT(retained.size() == 5 && retained[4].asStringView() == "five");
}

void typedStopPreservesAcceptedReply() {
    const auto first = encode("first");
    const auto wire = first + encode("later");
    const auto result = runScenario(receiverOptions(4), [&](const auto& connection, PacketView view) {
        GAMENET_TEST_ASSERT(connection->trySend(encode(view.asStringView())) == gamenet::net::TcpSendResult::Accepted);
        return FrameVisitAction::Stop;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, first, true); }, wire.size());
    GAMENET_TEST_ASSERT(result.handlerCalls == 1);
    GAMENET_TEST_ASSERT(result.reason == FrameReceiverStopReason::Requested);
    GAMENET_TEST_ASSERT(result.consumedBytes == first.size());
    GAMENET_TEST_ASSERT(result.discardedBytes == wire.size() - first.size());
}

void queuedContinuationRetiresAfterReentrantStop() {
    gamenet::net::EventLoop* loop = nullptr;
    PacketFramerViewReceiver* receiver = nullptr;
    const auto wire = encode("one") + encode("two") + encode("three");
    const auto result = runScenario(receiverOptions(1), [&](const auto&, PacketView) {
        GAMENET_TEST_ASSERT(loop->tryQueueInLoop([&] { receiver->stop(); }));
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); },
        wire.size(), {}, &loop, &receiver);
    GAMENET_TEST_ASSERT(result.handlerCalls == 1);
    GAMENET_TEST_ASSERT(result.continuationPosts == 1);
    GAMENET_TEST_ASSERT(result.reason == FrameReceiverStopReason::Requested);
    GAMENET_TEST_ASSERT(result.consumedBytes + result.discardedBytes == wire.size());

    const auto stopped = runScenario(receiverOptions(4), [&](const auto&, PacketView) {
        gamenet::net::Buffer other;
        other.append(std::string_view("unrelated"));
        receiver->onMessage(other);
        GAMENET_TEST_ASSERT(other.readableBytes() == 9);
        receiver->stop();
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); },
        wire.size(), {}, nullptr, &receiver);
    GAMENET_TEST_ASSERT(stopped.handlerCalls == 1);
    GAMENET_TEST_ASSERT(stopped.reentrantDispatchRejections == 1);
    GAMENET_TEST_ASSERT(stopped.consumedBytes + stopped.discardedBytes == wire.size());
}

void rejectionAndHandlerFailuresCloseInput() {
    gamenet::net::EventLoop* loop = nullptr;
    const auto wire = encode("one") + encode("two");
    const auto rejected = runScenario(receiverOptions(1), [&](const auto&, PacketView) {
        GAMENET_TEST_ASSERT(loop->tryQueueInLoop([] {}));
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); },
        wire.size(), {.maxPendingFunctors = 1, .reservedPendingFunctors = 0,
                      .maxFunctorsPerIteration = 1}, &loop);
    GAMENET_TEST_ASSERT(rejected.continuationRejections == 1 && rejected.handlerCalls == 1);
    GAMENET_TEST_ASSERT(rejected.reason == FrameReceiverStopReason::ContinuationRejected);

    const auto failed = runScenario(receiverOptions(), [](const auto&, PacketView) -> FrameVisitAction {
        throw std::runtime_error("receiver handler failure");
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); }, wire.size());
    GAMENET_TEST_ASSERT(failed.handlerExceptions == 1 && failed.handlerCalls == 1);
    GAMENET_TEST_ASSERT(failed.discardedBytes == wire.size());
    GAMENET_TEST_ASSERT(failed.reason == FrameReceiverStopReason::HandlerException);

    auto options = receiverOptions();
    options.maxHandlerWallTime = 1ms;
    const auto overrun = runScenario(options, [](const auto&, PacketView) {
        std::this_thread::sleep_for(10ms);
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); }, wire.size());
    GAMENET_TEST_ASSERT(overrun.handlerOverruns == 1 && overrun.handlerCalls == 1);
}

void connectionCloseFromHandlerKeepsTheBorrowAlive() {
    const auto wire = encode("first") + encode("later");
    const auto result = runScenario(receiverOptions(4), [](const auto& connection, PacketView view) {
        const auto owned = view.retain();
        connection->forceClose();
        GAMENET_TEST_ASSERT(view.asStringView() == owned.asStringView());
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); }, wire.size());
    GAMENET_TEST_ASSERT(result.handlerCalls == 1);
    GAMENET_TEST_ASSERT(result.consumedBytes + result.discardedBytes == wire.size());
}

void serverStopCanReenterFromTheBorrowedHandler() {
    gamenet::net::TcpServer* server = nullptr;
    PacketFramerViewReceiver* receiver = nullptr;
    gamenet::net::TcpServerStopFuture stop;
    const auto wire = encode("first") + encode("later");
    const auto result = runScenario(receiverOptions(4), [&](const auto&, PacketView view) {
        const auto owned = view.retain();
        receiver->stop();
        stop = server->stopGracefully({.drainTimeout = 0ms});
        GAMENET_TEST_ASSERT(view.asStringView() == owned.asStringView());
        return FrameVisitAction::Continue;
    }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); },
        wire.size(), {}, nullptr, &receiver, &server);
    GAMENET_TEST_ASSERT(result.handlerCalls == 1);
    GAMENET_TEST_ASSERT(stop.valid() && stop.wait_for(0s) == std::future_status::ready);
}

void protocolFaultsCloseBeforeHandlers() {
    for (const auto& wire : {std::string("\x00\x00\x00\x11", 4), std::string(129, '\0')}) {
        const auto result = runScenario(receiverOptions(), [](const auto&, PacketView) {
            GAMENET_TEST_FAIL("protocol fault reached the handler");
            return FrameVisitAction::Continue;
        }, [&](auto socket, auto&) { return writeBytes(socket, wire) && readBytes(socket, {}, true); }, wire.size());
        GAMENET_TEST_ASSERT(result.protocolFailures == 1 && result.handlerCalls == 0);
        GAMENET_TEST_ASSERT(result.reason == FrameReceiverStopReason::ProtocolError);
        GAMENET_TEST_ASSERT(result.discardedBytes == wire.size());
    }
}

}  // namespace

void runPacketFramerViewReceiverContracts() {
    realInputRetainsPartialFramesAndEchoesInOrder();
    typedStopPreservesAcceptedReply();
    queuedContinuationRetiresAfterReentrantStop();
    rejectionAndHandlerFailuresCloseInput();
    connectionCloseFromHandlerKeepsTheBorrowAlive();
    serverStopCanReenterFromTheBorrowedHandler();
    protocolFaultsCloseBeforeHandlers();
}
