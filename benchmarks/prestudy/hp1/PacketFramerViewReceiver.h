// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Optional owner-local TCP composition of the HP1 borrowed-view prototype.
// See hp1_packet_framer_view_prestudy.intent.md and ownership_rules.md 11.1.1.

#include "hp1/PacketFramerView.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/TcpConnection.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace gamenet::experimental::hp1 {

enum class FrameReceiverPhase { Running, Stopping, Stopped };
enum class FrameReceiverStopReason {
    None, Requested, ConnectionClosed, ProtocolError, HandlerException,
    HandlerOverrun, ContinuationRejected,
};

struct FrameReceiverSnapshot {
    FrameReceiverPhase phase{FrameReceiverPhase::Running};
    FrameReceiverStopReason reason{FrameReceiverStopReason::None};
    std::uint64_t handlerCalls{};
    std::uint64_t consumedBytes{};
    std::uint64_t discardedBytes{};
    std::uint64_t continuationPosts{};
    std::uint64_t continuationRejections{};
    std::uint64_t handlerExceptions{};
    std::uint64_t handlerOverruns{};
    std::uint64_t protocolFailures{};
    std::uint64_t reentrantDispatchRejections{};
    std::size_t maxHandlersPerDispatch{};
    std::size_t bufferedBytes{};
    bool continuationQueued{};
    bool dispatchActive{};

    bool settled() const noexcept {
        return phase == FrameReceiverPhase::Stopped && !continuationQueued &&
            !dispatchActive && bufferedBytes == 0;
    }
};

struct FrameReceiverOptions {
    protocol::PacketFramerOptions framing{};
    std::chrono::steady_clock::duration maxHandlerWallTime{
        std::chrono::milliseconds(5)};
};

class PacketFramerViewReceiver
    : public std::enable_shared_from_this<PacketFramerViewReceiver> {
public:
    using Handler = std::function<FrameVisitAction(const net::TcpConnectionPtr&, PacketView)>;

    static std::shared_ptr<PacketFramerViewReceiver> create(
        const net::TcpConnectionPtr& connection, Handler handler,
        FrameReceiverOptions options = {});

    PacketFramerViewReceiver(const PacketFramerViewReceiver&) = delete;
    PacketFramerViewReceiver& operator=(const PacketFramerViewReceiver&) = delete;

    // All methods are owner-only. Pass this connection's message-callback Buffer.
    void onMessage(net::Buffer& input);
    void stop();
    FrameReceiverSnapshot snapshot() const;

private:
    PacketFramerViewReceiver(
        const net::TcpConnectionPtr& connection, Handler handler,
        FrameReceiverOptions options);
    void assertOwner() const;
    void dispatch(const net::TcpConnectionPtr& connection);
    void requestStop(FrameReceiverStopReason reason) noexcept;
    void finishStop(const net::TcpConnectionPtr& connection);
    void discardInput();

    std::weak_ptr<net::TcpConnection> connection_;
    net::EventLoop* loop_;
    net::EventLoopExecutor owner_;
    PacketFramerViewPrototype framer_;
    Handler handler_;
    FrameReceiverOptions options_;
    net::Buffer* input_{};
    FrameReceiverSnapshot state_;
};

}  // namespace gamenet::experimental::hp1
