// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerViewReceiver.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace gamenet::experimental::hp1 {

std::shared_ptr<PacketFramerViewReceiver> PacketFramerViewReceiver::create(
    const net::TcpConnectionPtr& connection, Handler handler,
    FrameReceiverOptions options) {
    if (!connection) throw std::invalid_argument("HP1 receiver requires a connection");
    connection->getLoop()->assertInLoopThread();
    return std::shared_ptr<PacketFramerViewReceiver>(
        new PacketFramerViewReceiver(connection, std::move(handler), std::move(options)));
}

PacketFramerViewReceiver::PacketFramerViewReceiver(
    const net::TcpConnectionPtr& connection, Handler handler,
    FrameReceiverOptions options)
    : connection_(connection), loop_(connection->getLoop()), owner_(loop_->executor()),
      framer_(options.framing), handler_(std::move(handler)), options_(std::move(options)) {
    if (!handler_ || options_.maxHandlerWallTime <= std::chrono::steady_clock::duration::zero()) {
        throw std::invalid_argument("HP1 receiver requires a handler and positive wall-time budget");
    }
}

void PacketFramerViewReceiver::assertOwner() const {
    loop_->assertInLoopThread();
}

void PacketFramerViewReceiver::onMessage(net::Buffer& input) {
    assertOwner();
    const auto guard = shared_from_this();
    if (state_.dispatchActive) {
        ++state_.reentrantDispatchRejections;
        return;
    }
    if (input_ != nullptr && input_ != &input) {
        throw std::invalid_argument("HP1 receiver cannot change its source Buffer");
    }
    input_ = &input;
    const auto connection = connection_.lock();
    if (!connection) throw std::logic_error("HP1 receiver connection expired");
    if (state_.phase != FrameReceiverPhase::Running || !connection->connected()) {
        requestStop(FrameReceiverStopReason::ConnectionClosed);
        finishStop(connection);
        return;
    }
    if (!state_.continuationQueued) dispatch(connection);
}

void PacketFramerViewReceiver::requestStop(FrameReceiverStopReason reason) noexcept {
    if (state_.phase == FrameReceiverPhase::Running) {
        state_.phase = FrameReceiverPhase::Stopping;
        state_.reason = reason;
    }
}

void PacketFramerViewReceiver::discardInput() {
    if (input_ == nullptr) return;
    state_.discardedBytes += input_->readableBytes();
    input_->retrieveAll();
}

void PacketFramerViewReceiver::finishStop(const net::TcpConnectionPtr& connection) {
    if (state_.dispatchActive) return;
    discardInput();
    state_.phase = FrameReceiverPhase::Stopped;
    if (connection->disconnected()) return;
    switch (state_.reason) {
        case FrameReceiverStopReason::Requested:
        case FrameReceiverStopReason::HandlerException:
        case FrameReceiverStopReason::ContinuationRejected:
            connection->shutdown();
            break;
        case FrameReceiverStopReason::ProtocolError:
        case FrameReceiverStopReason::HandlerOverrun:
            connection->forceClose();
            break;
        default:
            break;
    }
}

void PacketFramerViewReceiver::stop() {
    assertOwner();
    requestStop(FrameReceiverStopReason::Requested);
    if (state_.dispatchActive) return;
    if (const auto connection = connection_.lock()) finishStop(connection);
    else state_.phase = FrameReceiverPhase::Stopped;
}

FrameReceiverSnapshot PacketFramerViewReceiver::snapshot() const {
    assertOwner();
    auto result = state_;
    const auto connection = connection_.lock();
    result.bufferedBytes = connection && input_ ? input_->readableBytes() : 0;
    return result;
}

void PacketFramerViewReceiver::dispatch(const net::TcpConnectionPtr& connection) {
    if (state_.phase != FrameReceiverPhase::Running || !connection->connected()) {
        requestStop(FrameReceiverStopReason::ConnectionClosed);
        finishStop(connection);
        return;
    }
    state_.dispatchActive = true;
    std::size_t calls = 0;
    const auto result = framer_.visitFrames(
        std::span(reinterpret_cast<const std::byte*>(input_->peek()), input_->readableBytes()),
        [&](PacketView view) {
            ++state_.handlerCalls;
            ++calls;
            const auto started = std::chrono::steady_clock::now();
            FrameVisitAction action;
            try {
                action = handler_(connection, view);
            } catch (...) {
                ++state_.handlerExceptions;
                requestStop(FrameReceiverStopReason::HandlerException);
                throw;
            }
            if (std::chrono::steady_clock::now() - started > options_.maxHandlerWallTime) {
                ++state_.handlerOverruns;
                requestStop(FrameReceiverStopReason::HandlerOverrun);
            }
            if (action == FrameVisitAction::Stop) requestStop(FrameReceiverStopReason::Requested);
            if (!connection->connected()) requestStop(FrameReceiverStopReason::ConnectionClosed);
            return state_.phase == FrameReceiverPhase::Running
                ? FrameVisitAction::Continue : FrameVisitAction::Stop;
        });
    state_.dispatchActive = false;
    state_.maxHandlersPerDispatch = std::max(state_.maxHandlersPerDispatch, calls);
    state_.consumedBytes += result.consumedBytes;
    input_->retrieve(result.consumedBytes);
    if (framer_.faulted()) {
        ++state_.protocolFailures;
        requestStop(FrameReceiverStopReason::ProtocolError);
    }
    if (state_.phase != FrameReceiverPhase::Running) {
        finishStop(connection);
        return;
    }
    if (!result.needsContinuation) return;

    // The strong connection guard pins the Buffer object, not its readable span.
    const auto self = shared_from_this();
    const auto posted = owner_.post([self, connection] {
        self->state_.continuationQueued = false;
        self->dispatch(connection);
    });
    if (posted == net::PostResult::Accepted) {
        state_.continuationQueued = true;
        ++state_.continuationPosts;
    } else {
        ++state_.continuationRejections;
        requestStop(FrameReceiverStopReason::ContinuationRejected);
        finishStop(connection);
    }
}

}  // namespace gamenet::experimental::hp1
