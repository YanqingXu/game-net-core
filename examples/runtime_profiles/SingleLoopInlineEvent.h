#pragma once

#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/protocol/PacketFramer.h"
#include "gamenet/transport/TransportEndpoint.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gamenet::examples {

enum class SingleLoopInlineAction {
    Continue,
    Close,
};

struct SingleLoopInlineHandlerResult {
    SingleLoopInlineAction action{SingleLoopInlineAction::Continue};
    std::optional<std::string> reply;
};

using SingleLoopInlineHandler = std::function<SingleLoopInlineHandlerResult(
    gamenet::transport::TransportSessionId,
    std::string_view)>;

struct SingleLoopInlineEventOptions {
    gamenet::protocol::PacketFramerOptions framing{
        .maxPayloadBytes = 64U * 1024U,
        .maxBufferedBytes = 2U * (64U * 1024U + sizeof(std::uint32_t)),
        .maxFramesPerPush = 32,
        .maxFrameBytesPerPush = 256U * 1024U,
    };
    gamenet::net::TcpServerAdmissionOptions admission{};
    gamenet::net::TcpConnectionBackpressureOptions connectionBackpressure{};
    std::chrono::steady_clock::duration maxHandlerWallTime{
        std::chrono::milliseconds(5)};
};

struct SingleLoopInlineEventMetrics {
    std::uint64_t connectionsOpened{};
    std::uint64_t connectionsClosed{};
    std::uint64_t handlerCalls{};
    std::uint64_t repliesAccepted{};
    std::uint64_t handlerOverruns{};
    std::uint64_t handlerExceptions{};
    std::uint64_t continuationPosts{};
    std::uint64_t continuationRejections{};
    std::uint64_t outputOverloads{};
    std::uint64_t protocolFailures{};
    std::size_t maxHandlersPerDispatch{};
    std::uint64_t crossDomainHandoffs{};
};

class SingleLoopInlineEvent {
public:
    SingleLoopInlineEvent(
        gamenet::net::EventLoop* loop,
        const gamenet::net::InetAddress& listenAddress,
        SingleLoopInlineHandler handler,
        SingleLoopInlineEventOptions options = {});
    ~SingleLoopInlineEvent();

    SingleLoopInlineEvent(const SingleLoopInlineEvent&) = delete;
    SingleLoopInlineEvent& operator=(const SingleLoopInlineEvent&) = delete;

    void start();
    void stop();
    gamenet::net::TcpServerStopFuture stopGracefully(
        gamenet::net::TcpServerStopOptions options = {});

    const gamenet::net::InetAddress& listenAddress() const noexcept;
    SingleLoopInlineEventMetrics metrics() const;

private:
    struct CallbackState;
    struct ConnectionState;

    static void handleFramerResult(
        const std::shared_ptr<ConnectionState>& state,
        gamenet::protocol::FrameResult result);

    gamenet::net::EventLoop* loop_;
    std::shared_ptr<CallbackState> callbackState_;
    gamenet::net::TcpServer server_;
    gamenet::net::TcpServerStopFuture stopFuture_;
    bool started_{false};
    bool stopped_{false};
};

}  // namespace gamenet::examples
