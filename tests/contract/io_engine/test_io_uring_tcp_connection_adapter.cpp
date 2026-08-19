#include "experimental/io_uring/IoUringTcpConnectionAdapter.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

namespace uring = gamenet::experimental::io_uring;

constexpr std::size_t kLowWater = 16U * 1024U;
constexpr std::size_t kHighWater = 64U * 1024U;
constexpr std::size_t kHardLimit = 5U * 1024U * 1024U;
constexpr std::size_t kPayloadBytes = 4U * 1024U * 1024U;

class OwnedFd {
public:
    explicit OwnedFd(int value = -1) noexcept : value_(value) {}
    ~OwnedFd() {
        if (value_ >= 0) ::close(value_);
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this == &other) return *this;
        if (value_ >= 0) ::close(value_);
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }

private:
    int value_;
};

struct TcpPair {
    OwnedFd connection;
    OwnedFd peer;
};

class TcpPairFactory {
public:
    TcpPairFactory()
        : listener_(::socket(
              AF_INET,
              SOCK_STREAM | SOCK_CLOEXEC,
              IPPROTO_TCP)) {
        GAMENET_TEST_ASSERT(listener_.get() >= 0);
        int enabled = 1;
        GAMENET_TEST_ASSERT(
            ::setsockopt(
                listener_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &enabled,
                sizeof(enabled)) == 0);
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_.sin_port = 0;
        GAMENET_TEST_ASSERT(
            ::bind(
                listener_.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) == 0);
        GAMENET_TEST_ASSERT(::listen(listener_.get(), 4) == 0);
        socklen_t length = sizeof(address_);
        GAMENET_TEST_ASSERT(
            ::getsockname(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&address_),
                &length) == 0);
    }

    TcpPair makePair(bool nonblockingConnection) {
        OwnedFd peer(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            IPPROTO_TCP));
        GAMENET_TEST_ASSERT(peer.get() >= 0);
        GAMENET_TEST_ASSERT(
            ::connect(
                peer.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) == 0);
        const int flags = SOCK_CLOEXEC |
            (nonblockingConnection ? SOCK_NONBLOCK : 0);
        OwnedFd connection(::accept4(
            listener_.get(),
            nullptr,
            nullptr,
            flags));
        GAMENET_TEST_ASSERT(connection.get() >= 0);
        setNonblocking(peer.get());
        int sendBuffer = 4096;
        GAMENET_TEST_ASSERT(
            ::setsockopt(
                connection.get(),
                SOL_SOCKET,
                SO_SNDBUF,
                &sendBuffer,
                sizeof(sendBuffer)) == 0);
        return {
            .connection = std::move(connection),
            .peer = std::move(peer),
        };
    }

private:
    static void setNonblocking(int descriptor) {
        const int flags = ::fcntl(descriptor, F_GETFL, 0);
        GAMENET_TEST_ASSERT(flags >= 0);
        GAMENET_TEST_ASSERT(
            ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0);
    }

    OwnedFd listener_;
    sockaddr_in address_{};
};

uring::IoUringTcpConnectionHubOptions hubOptions() {
    return {
        .pump = {
            .engine = {
                .entries = 256,
                .maxOperations = 8,
                .maxCompletionsPerWait = 64,
                .maxBytesPerOperation = 16U * 1024U,
                .maxOwnedBytes = 64U * 1024U,
            },
            .maxNoticesPerTurn = 8,
        },
        .maxConnections = 2,
        .maxTotalPendingSendBytes = 2U * kHardLimit,
        .maxReceiveBytes = 4U * 1024U,
        .maxSendBytesPerOperation = 16U * 1024U,
        .maxPendingSendBytesPerConnection = kHardLimit,
        .maxPendingSendSegmentsPerConnection = 8,
    };
}

uring::IoUringTcpConnectionAdapterOptions adapterOptions() {
    return {
        .lowWaterMarkBytes = kLowWater,
        .highWaterMarkBytes = kHighWater,
        .hardLimitBytes = kHardLimit,
    };
}

void sendAll(int descriptor, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = ::send(
            descriptor,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        GAMENET_TEST_ASSERT(false && "peer send failed");
    }
}

void drainBounded(int descriptor, std::size_t& bytes) {
    std::array<char, 64U * 1024U> buffer{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto count =
            ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            bytes += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return;
        if (errno == EINTR) {
            --attempt;
            continue;
        }
        GAMENET_TEST_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

void drainAvailable(int descriptor, std::size_t& bytes) {
    std::array<char, 64U * 1024U> buffer{};
    while (true) {
        const auto count =
            ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            bytes += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return;
        if (errno == EINTR) continue;
        GAMENET_TEST_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

void testProductionAndAdapterCommonSemantics() {
    gamenet::net::EventLoop loop;
    TcpPairFactory factory;
    auto readinessPair = factory.makePair(true);
    auto completionPair = factory.makePair(false);
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubStop;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        hubOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubStop = summary;
            loop.quit();
        });
    uring::IoUringTcpConnectionAdapter adapter(
        &loop,
        &hub,
        adapterOptions());

    const gamenet::net::InetAddress readinessLocal(
        gamenet::net::sockets::getLocalAddr(readinessPair.connection.get()));
    const gamenet::net::InetAddress readinessPeer(
        gamenet::net::sockets::getPeerAddr(readinessPair.connection.get()));
    auto readiness = std::make_shared<gamenet::net::TcpConnection>(
        &loop,
        "ioe-x6-production-readiness",
        readinessPair.connection.release(),
        readinessLocal,
        readinessPeer);
    readiness->setBackpressureOptions({
        .lowWaterMarkBytes = kLowWater,
        .highWaterMarkBytes = kHighWater,
        .hardLimitBytes = kHardLimit,
        .maxInputBufferBytes = 64U * 1024U,
    });

    int highWaterCallbacks[2]{};
    int writeCompleteCallbacks[2]{};
    int messageCallbacks[2]{};
    int closeInfoCallbacks[2]{};
    int closeCallbacks[2]{};
    bool sendReturned = false;
    bool peerInputSent = false;
    bool draining = false;
    bool timedOut = false;
    std::size_t peerReadBytes[2]{};
    std::optional<gamenet::net::TcpConnectionCloseInfo> closeInfos[2];
    std::shared_future<uring::IoUringTcpConnectionAdapterStopSummary>
        adapterFuture;

    auto maybeStartPeerInput = [&] {
        if (highWaterCallbacks[0] != 1 || highWaterCallbacks[1] != 1) {
            return;
        }
        loop.runAfter(10ms, [&] {
            GAMENET_TEST_ASSERT(
                readiness->readingPausedByBackpressure());
            GAMENET_TEST_ASSERT(
                adapter.readingPausedByBackpressure());
            sendAll(readinessPair.peer.get(), "ping");
            sendAll(completionPair.peer.get(), "ping");
            peerInputSent = true;
            loop.runAfter(25ms, [&] {
                GAMENET_TEST_ASSERT(messageCallbacks[0] == 0);
                GAMENET_TEST_ASSERT(messageCallbacks[1] == 0);
                draining = true;
            });
        });
    };

    auto maybeCloseReadiness = [&] {
        if (messageCallbacks[0] == 1 && writeCompleteCallbacks[0] >= 2 &&
            readiness->connected()) {
            GAMENET_TEST_ASSERT(
                readiness->tryForceClose() ==
                gamenet::net::PostResult::Accepted);
        }
    };
    auto maybeCloseAdapter = [&] {
        if (messageCallbacks[1] == 1 && writeCompleteCallbacks[1] >= 2 &&
            adapter.connected()) {
            GAMENET_TEST_ASSERT(
                adapter.tryForceClose() ==
                gamenet::net::PostResult::Accepted);
        }
    };
    auto maybeStopHub = [&] {
        if (closeCallbacks[0] == 1 && closeCallbacks[1] == 1) {
            GAMENET_TEST_ASSERT(hub.stop());
        }
    };

    readiness->setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            std::size_t bytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(sendReturned);
            GAMENET_TEST_ASSERT(bytes >= kHighWater);
            GAMENET_TEST_ASSERT(
                connection->readingPausedByBackpressure());
            ++highWaterCallbacks[0];
            maybeStartPeerInput();
        },
        kHighWater);
    adapter.setHighWaterMarkCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::size_t bytes) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(sendReturned);
            GAMENET_TEST_ASSERT(bytes >= kHighWater);
            GAMENET_TEST_ASSERT(
                connection.readingPausedByBackpressure());
            ++highWaterCallbacks[1];
            maybeStartPeerInput();
        });

    readiness->setWriteCompleteCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++writeCompleteCallbacks[0];
            if (writeCompleteCallbacks[0] == 1) {
                GAMENET_TEST_ASSERT(
                    connection->trySend("w") ==
                    gamenet::net::TcpSendResult::Accepted);
            }
            maybeCloseReadiness();
        });
    adapter.setWriteCompleteCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            ++writeCompleteCallbacks[1];
            if (writeCompleteCallbacks[1] == 1) {
                GAMENET_TEST_ASSERT(
                    connection.trySend("w") ==
                    gamenet::net::TcpSendResult::Accepted);
            }
            maybeCloseAdapter();
        });

    readiness->setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* input) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(peerInputSent);
            GAMENET_TEST_ASSERT(
                !connection->readingPausedByBackpressure());
            GAMENET_TEST_ASSERT(input->retrieveAllAsString() == "ping");
            ++messageCallbacks[0];
            GAMENET_TEST_ASSERT(
                connection->trySend("m") ==
                gamenet::net::TcpSendResult::Accepted);
            maybeCloseReadiness();
        });
    adapter.setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            std::string_view payload) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(peerInputSent);
            GAMENET_TEST_ASSERT(
                !connection.readingPausedByBackpressure());
            GAMENET_TEST_ASSERT(payload == "ping");
            ++messageCallbacks[1];
            GAMENET_TEST_ASSERT(
                connection.trySend("m") ==
                gamenet::net::TcpSendResult::Accepted);
            maybeCloseAdapter();
        });

    readiness->setCloseInfoCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(connection->socketClosed());
            GAMENET_TEST_ASSERT(
                connection->closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            closeInfos[0] = info;
            ++closeInfoCallbacks[0];
        });
    adapter.setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter& connection,
            const gamenet::net::TcpConnectionCloseInfo& info) {
            GAMENET_TEST_ASSERT(
                adapterFuture.wait_for(0s) == std::future_status::ready);
            GAMENET_TEST_ASSERT(
                connection.closePhase() ==
                gamenet::net::TcpConnectionClosePhase::Closed);
            closeInfos[1] = info;
            ++closeInfoCallbacks[1];
        });
    readiness->setCloseCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            connection->connectDestroyed();
            ++closeCallbacks[0];
            maybeStopHub();
        });
    adapter.setCloseCallback(
        [&](uring::IoUringTcpConnectionAdapter&) {
            ++closeCallbacks[1];
            maybeStopHub();
        });

    loop.runEvery(1ms, [&] {
        if (!draining) return;
        drainBounded(readinessPair.peer.get(), peerReadBytes[0]);
        drainBounded(completionPair.peer.get(), peerReadBytes[1]);
    });
    loop.runAfter(0ms, [&] {
        readiness->connectEstablished();
        GAMENET_TEST_ASSERT(
            adapter.establish(completionPair.connection.release()) ==
            uring::IoUringTcpHubAddResult::Accepted);
        adapterFuture = adapter.stopFuture();
        GAMENET_TEST_ASSERT(readiness->connected());
        GAMENET_TEST_ASSERT(adapter.connected());
        GAMENET_TEST_ASSERT(
            readiness->trySend("") == gamenet::net::TcpSendResult::Accepted);
        GAMENET_TEST_ASSERT(
            adapter.trySend("") == gamenet::net::TcpSendResult::Accepted);
        const std::string oversized(kHardLimit + 1U, 'o');
        GAMENET_TEST_ASSERT(
            readiness->trySend(oversized) ==
            gamenet::net::TcpSendResult::Overloaded);
        GAMENET_TEST_ASSERT(
            adapter.trySend(oversized) ==
            gamenet::net::TcpSendResult::Overloaded);
        const std::string payload(kPayloadBytes, 'p');
        GAMENET_TEST_ASSERT(
            readiness->trySend(payload) ==
            gamenet::net::TcpSendResult::Accepted);
        GAMENET_TEST_ASSERT(
            adapter.trySend(payload) ==
            gamenet::net::TcpSendResult::Accepted);
        sendReturned = true;
    });
    loop.runAfter(5s, [&] {
        timedOut = true;
        (void)readiness->tryForceClose();
        (void)adapter.tryForceClose();
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    drainAvailable(readinessPair.peer.get(), peerReadBytes[0]);
    drainAvailable(completionPair.peer.get(), peerReadBytes[1]);
    for (int index = 0; index < 2; ++index) {
        GAMENET_TEST_ASSERT(highWaterCallbacks[index] == 1);
        GAMENET_TEST_ASSERT(writeCompleteCallbacks[index] >= 2);
        GAMENET_TEST_ASSERT(messageCallbacks[index] == 1);
        GAMENET_TEST_ASSERT(closeInfoCallbacks[index] == 1);
        GAMENET_TEST_ASSERT(closeCallbacks[index] == 1);
        GAMENET_TEST_ASSERT(closeInfos[index].has_value());
        GAMENET_TEST_ASSERT(
            closeInfos[index]->reason ==
            gamenet::net::TcpConnectionCloseReason::ForcedShutdown);
        GAMENET_TEST_ASSERT(closeInfos[index]->nativeError == 0);
        GAMENET_TEST_ASSERT(peerReadBytes[index] >= kPayloadBytes);
    }
    GAMENET_TEST_ASSERT(readiness->disconnected());
    GAMENET_TEST_ASSERT(readiness->pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(adapter.disconnected());
    GAMENET_TEST_ASSERT(adapter.pendingOutputBytes() == 0);
    GAMENET_TEST_ASSERT(
        readiness->trySend("late") == gamenet::net::TcpSendResult::Closed);
    GAMENET_TEST_ASSERT(
        adapter.trySend("late") == gamenet::net::TcpSendResult::Closed);

    const auto adapterStop = adapterFuture.get();
    GAMENET_TEST_ASSERT(adapterStop.established);
    GAMENET_TEST_ASSERT(adapterStop.transport.socketClosed);
    GAMENET_TEST_ASSERT(adapterStop.adapter.overloadRejections == 1);
    GAMENET_TEST_ASSERT(adapterStop.adapter.pendingOutputBytes == 0);
    GAMENET_TEST_ASSERT(adapterStop.transport.connection.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(hubStop.has_value());
    GAMENET_TEST_ASSERT(hubStop->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubStop->hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(hubStop->hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(hubStop->hub.pendingSendBytes == 0);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.ownedBytes == 0);
}

void testObserverDestructionRetainsPhysicalObligation() {
    gamenet::net::EventLoop loop;
    TcpPairFactory factory;
    auto pair = factory.makePair(false);
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubStop;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        hubOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubStop = summary;
            loop.quit();
        });
    int observerCallbacks = 0;
    auto adapter = std::make_unique<uring::IoUringTcpConnectionAdapter>(
        &loop,
        &hub,
        adapterOptions());
    adapter->setMessageCallback(
        [&](uring::IoUringTcpConnectionAdapter&, std::string_view) {
            ++observerCallbacks;
        });
    adapter->setCloseInfoCallback(
        [&](uring::IoUringTcpConnectionAdapter&,
            const gamenet::net::TcpConnectionCloseInfo&) {
            ++observerCallbacks;
        });
    adapter->setCloseCallback(
        [&](uring::IoUringTcpConnectionAdapter&) {
            ++observerCallbacks;
        });
    GAMENET_TEST_ASSERT(
        adapter->establish(pair.connection.release()) ==
        uring::IoUringTcpHubAddResult::Accepted);
    auto future = adapter->stopFuture();
    adapter.reset();

    bool terminalObserved = false;
    bool timedOut = false;
    loop.runEvery(1ms, [&] {
        if (terminalObserved ||
            future.wait_for(0s) != std::future_status::ready) {
            return;
        }
        terminalObserved = true;
        const auto summary = future.get();
        GAMENET_TEST_ASSERT(summary.established);
        GAMENET_TEST_ASSERT(
            summary.closeInfo.reason ==
            gamenet::net::TcpConnectionCloseReason::ForcedShutdown);
        GAMENET_TEST_ASSERT(summary.transport.socketClosed);
        GAMENET_TEST_ASSERT(summary.transport.connection.pendingSendBytes == 0);
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(3s, [&] {
        timedOut = true;
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(!timedOut);
    GAMENET_TEST_ASSERT(terminalObserved);
    GAMENET_TEST_ASSERT(observerCallbacks == 0);
    GAMENET_TEST_ASSERT(hubStop.has_value());
    GAMENET_TEST_ASSERT(hubStop->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubStop->hub.socketCloseCount == 1);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.activeOperations == 0);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.readyNotices == 0);
    GAMENET_TEST_ASSERT(hubStop->pump.engine.ownedBytes == 0);
}

}  // namespace

int main() {
    testProductionAndAdapterCommonSemantics();
    testObserverDestructionRetainsPhysicalObligation();
    return 0;
}
