#include <gamenet/core/DispatchResult.h>
#include <gamenet/core/base/Logger.h>
#include <gamenet/core/base/Timestamp.h>
#include <gamenet/core/base/noncopyable.h>
#include <gamenet/core/net/Acceptor.h>
#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/CallbackException.h>
#include <gamenet/core/net/Callbacks.h>
#include <gamenet/core/net/Channel.h>
#include <gamenet/core/net/Connector.h>
#include <gamenet/core/net/ConnectorOptions.h>
#include <gamenet/core/net/DeadlineQueue.h>
#include <gamenet/core/net/EventLoop.h>
#include <gamenet/core/net/EventLoopExecutor.h>
#include <gamenet/core/net/EventLoopMetrics.h>
#include <gamenet/core/net/EventLoopThread.h>
#include <gamenet/core/net/EventLoopThreadPool.h>
#include <gamenet/core/net/InetAddress.h>
#include <gamenet/core/net/NetworkMemoryRetention.h>
#include <gamenet/core/net/Poller.h>
#include <gamenet/core/net/PostResult.h>
#include <gamenet/core/net/Socket.h>
#include <gamenet/core/net/SocketTypes.h>
#include <gamenet/core/net/SocketsOps.h>
#include <gamenet/core/net/TcpClient.h>
#include <gamenet/core/net/TcpClientControl.h>
#include <gamenet/core/net/TcpConnection.h>
#include <gamenet/core/net/TcpConnectionClose.h>
#include <gamenet/core/net/TcpConnectionOptions.h>
#include <gamenet/core/net/TcpOutputMemoryBudget.h>
#include <gamenet/core/net/TcpServer.h>
#include <gamenet/core/net/TimerId.h>
#include <gamenet/core/net/TimerOptions.h>
#include <gamenet/core/net/TimerQueue.h>

#include <chrono>
#include <stdexcept>

int main() {
    gamenet::net::Buffer buffer;
    buffer.append("ok", 2);

    gamenet::net::EventLoop loop;
    gamenet::net::DeadlineQueue deadlines(&loop);
    const auto now = gamenet::base::now();
    const auto token = deadlines.schedule(7, now - std::chrono::seconds(1));
    const auto expired = deadlines.advance(now);
    const auto timerAdmission =
        loop.tryRunAfter(std::chrono::hours(1), [] {});
    const auto timerCancellation = loop.tryCancel(timerAdmission.timerId);

    gamenet::net::TcpOutputMemoryBudget outputBudget;
    const auto outputSnapshot = outputBudget.snapshot();
    const auto retained =
        gamenet::net::networkFixedStorageRetentionSnapshot();
    const gamenet::net::RepeatingTimerOptions timerOptions{};
    const gamenet::net::TcpConnectionBackpressureOptions connectionOptions{};
    const gamenet::net::EventLoopCallbackException callbackFailure{};
    const gamenet::net::TcpConnectionCloseInfo closeInfo{};
    const gamenet::net::TcpClientControl detachedControl;
    const auto dispatch =
        gamenet::dispatchResult(gamenet::net::PostResult::Accepted);

    if (buffer.retrieveAllAsString() != "ok" || !token.valid() ||
        expired.expired.size() != 1 || outputSnapshot.pendingBytes != 0 ||
        retained.peakTotalRetainedBytes < retained.totalRetainedBytes ||
        timerAdmission.result != gamenet::net::PostResult::Accepted ||
        !timerAdmission.timerId.valid() ||
        timerCancellation != gamenet::net::PostResult::Accepted ||
        timerOptions.mode != gamenet::net::RepeatingTimerMode::FixedDelay ||
        connectionOptions.hardLimitBytes == 0 || callbackFailure.exception ||
        closeInfo.reason != gamenet::net::TcpConnectionCloseReason::InternalError ||
        detachedControl.available() ||
        detachedControl.tryStop() != gamenet::net::PostResult::OwnerUnavailable ||
        dispatch != gamenet::DispatchResult::Accepted) {
        return 1;
    }

    return 0;
}
