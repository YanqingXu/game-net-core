#pragma once

#include "IoUringTcpConnectionHub.h"

#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/TcpConnectionClose.h"
#include "gamenet/core/net/TcpConnectionOptions.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string_view>

namespace gamenet::experimental::io_uring {

struct IoUringTcpConnectionAdapterOptions {
    std::size_t lowWaterMarkBytes{16U * 1024U};
    std::size_t highWaterMarkBytes{64U * 1024U};
    std::size_t hardLimitBytes{4U * 1024U * 1024U};

    void validate() const;
};

struct IoUringTcpConnectionAdapterMetrics {
    std::uint64_t sendAdmissions{};
    std::uint64_t overloadRejections{};
    std::uint64_t ownerLimitRejections{};
    std::uint64_t highWaterNotifications{};
    std::uint64_t writeCompleteNotifications{};
    std::uint64_t droppedNotifications{};
    std::uint64_t callbackFailures{};
    std::size_t pendingOutputBytes{};
    std::size_t peakPendingOutputBytes{};
    bool readingPaused{};
};

struct IoUringTcpConnectionAdapterStopSummary {
    gamenet::net::TcpConnectionCloseInfo closeInfo{};
    IoUringTcpConnectionAdapterMetrics adapter{};
    IoUringTcpConnectionHubConnectionStopSummary transport{};
    bool established{};
};

class IoUringTcpConnectionAdapterImpl;

// Source-private IOE-X6 bridge that projects one Hub route onto the existing
// TCP send/close vocabulary. It is not installed and does not select or replace
// the production TcpConnection backend.
class IoUringTcpConnectionAdapter {
public:
    using MessageCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::string_view)>;
    using HighWaterMarkCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        std::size_t)>;
    using WriteCompleteCallback =
        std::function<void(IoUringTcpConnectionAdapter&)>;
    using CloseInfoCallback = std::function<void(
        IoUringTcpConnectionAdapter&,
        const gamenet::net::TcpConnectionCloseInfo&)>;
    using CloseCallback =
        std::function<void(IoUringTcpConnectionAdapter&)>;

    IoUringTcpConnectionAdapter(
        gamenet::net::EventLoop* ownerLoop,
        IoUringTcpConnectionHub* hub,
        IoUringTcpConnectionAdapterOptions options = {});
    ~IoUringTcpConnectionAdapter();
    IoUringTcpConnectionAdapter(const IoUringTcpConnectionAdapter&) = delete;
    IoUringTcpConnectionAdapter& operator=(
        const IoUringTcpConnectionAdapter&) = delete;

    void setMessageCallback(MessageCallback callback);
    void setHighWaterMarkCallback(HighWaterMarkCallback callback);
    void setWriteCompleteCallback(WriteCompleteCallback callback);
    void setCloseInfoCallback(CloseInfoCallback callback);
    void setCloseCallback(CloseCallback callback);

    // Socket ownership transfers on every call because the Hub consumes both
    // accepted and rejected descriptors. Configuration is sealed afterward.
    IoUringTcpHubAddResult establish(
        gamenet::net::SocketFd establishedSocket);
    gamenet::net::TcpSendResult trySend(std::string_view payload);
    gamenet::net::PostResult tryForceClose();

    bool connected() const;
    bool disconnected() const;
    std::size_t pendingOutputBytes() const;
    bool readingPausedByBackpressure() const;
    gamenet::net::TcpConnectionClosePhase closePhase() const;
    std::optional<gamenet::net::TcpConnectionCloseInfo> closeInfo() const;
    IoUringTcpConnectionAdapterMetrics metrics() const;
    std::shared_future<IoUringTcpConnectionAdapterStopSummary>
    stopFuture() const;

private:
    std::unique_ptr<IoUringTcpConnectionAdapterImpl> impl_;
};

}  // namespace gamenet::experimental::io_uring
