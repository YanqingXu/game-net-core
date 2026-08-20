#include "experimental/io_uring/IoUringTcpConnectionHub.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "../../support/TestAssert.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

namespace uring = gamenet::experimental::io_uring;

class OwnedFd {
public:
    explicit OwnedFd(int value = -1) noexcept : value_(value) {}
    ~OwnedFd() { close(); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this == &other) return *this;
        close();
        value_ = std::exchange(other.value_, -1);
        return *this;
    }

    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
    bool valid() const noexcept { return value_ >= 0; }
    void close() noexcept {
        if (value_ < 0) return;
        ::close(value_);
        value_ = -1;
    }

private:
    int value_;
};

struct ListenerEndpoint {
    OwnedFd socket;
    sockaddr_in address{};
};

ListenerEndpoint makeListener(int backlog = 32) {
    ListenerEndpoint endpoint{
        .socket = OwnedFd(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            IPPROTO_TCP)),
    };
    GAMENET_TEST_ASSERT(endpoint.socket.valid());
    int enabled = 1;
    GAMENET_TEST_ASSERT(
        ::setsockopt(
            endpoint.socket.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            sizeof(enabled)) == 0);
    endpoint.address.sin_family = AF_INET;
    endpoint.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.address.sin_port = 0;
    GAMENET_TEST_ASSERT(
        ::bind(
            endpoint.socket.get(),
            reinterpret_cast<const sockaddr*>(&endpoint.address),
            sizeof(endpoint.address)) == 0);
    GAMENET_TEST_ASSERT(::listen(endpoint.socket.get(), backlog) == 0);
    socklen_t length = sizeof(endpoint.address);
    GAMENET_TEST_ASSERT(
        ::getsockname(
            endpoint.socket.get(),
            reinterpret_cast<sockaddr*>(&endpoint.address),
            &length) == 0);
    return endpoint;
}

OwnedFd connectClient(const sockaddr_in& address) {
    OwnedFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    GAMENET_TEST_ASSERT(client.valid());
    while (::connect(
               client.get(),
               reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        GAMENET_TEST_ASSERT(errno == EINTR);
    }
    const auto flags = ::fcntl(client.get(), F_GETFL, 0);
    GAMENET_TEST_ASSERT(flags >= 0);
    GAMENET_TEST_ASSERT(
        ::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK) == 0);
    return client;
}

OwnedFd connectClient(const gamenet::net::InetAddress& address) {
    GAMENET_TEST_ASSERT(address.isIpv4());
    return connectClient(address.getSockAddrInet());
}

void sendAll(int descriptor, std::string_view payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto count = ::send(
            descriptor,
            payload.data() + offset,
            payload.size() - offset,
            MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        GAMENET_TEST_FAIL("failed to send test payload");
    }
}

void drainAvailable(OwnedFd& client, std::string& output) {
    if (!client.valid()) return;
    std::array<char, 64> buffer{};
    while (true) {
        const auto count =
            ::recv(client.get(), buffer.data(), buffer.size(), 0);
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            client.close();
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == ECONNRESET || errno == ENOTCONN) {
            client.close();
            return;
        }
        GAMENET_TEST_FAIL("unexpected client receive failure");
    }
}

uring::IoUringTcpConnectionHubOptions listenerOptions(
    std::size_t maxConnections = 4,
    std::size_t maxOperations = 16,
    std::size_t maxPendingAccepts = 2) {
    return {
        .pump = {
            .engine = {
                .entries = 32,
                .maxOperations = maxOperations,
                .maxCompletionsPerWait = 16,
                .maxBytesPerOperation = 16,
                .maxOwnedBytes = maxConnections * 32,
            },
            .maxNoticesPerTurn = 1,
        },
        .maxConnections = maxConnections,
        .maxTotalPendingSendBytes = maxConnections * 32,
        .maxReceiveBytes = 16,
        .maxSendBytesPerOperation = 16,
        .maxPendingSendBytesPerConnection = 32,
        .maxPendingSendSegmentsPerConnection = 4,
        .maxPendingAccepts = maxPendingAccepts,
    };
}

struct EchoObservation {
    std::string payload;
    bool connected{};
    bool stopped{};
};

EchoObservation runProductionEpollReference() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "ioe-x9-production-epoll-reference");
    EchoObservation observation;
    std::size_t disconnects = 0;
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult stopResult;

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                observation.connected = true;
            } else {
                ++disconnects;
            }
        });
    server.setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection,
            gamenet::net::Buffer* buffer) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            connection->send(buffer->retrieveAllAsString());
        });
    server.start();

    auto client = connectClient(server.listenAddress());
    constexpr std::string_view payload = "ioe-x9-echo";
    sendAll(client.get(), payload);
    const auto drainTimer = loop.runEvery(1ms, [&] {
        drainAvailable(client, observation.payload);
        if (observation.payload != payload || stopFuture.valid()) return;
        client.close();
        stopFuture = server.stopGracefully(
            gamenet::net::TcpServerStopOptions{.drainTimeout = 500ms});
    });
    const auto completionTimer = loop.runEvery(1ms, [&] {
        if (!stopFuture.valid() ||
            stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        stopResult = stopFuture.get();
        observation.stopped = true;
        loop.quit();
    });
    loop.runAfter(3s, [] {
        GAMENET_TEST_FAIL("production epoll reference timed out");
    });
    loop.loop();
    loop.cancel(drainTimer);
    loop.cancel(completionTimer);

    GAMENET_TEST_ASSERT(observation.connected);
    GAMENET_TEST_ASSERT(observation.payload == payload);
    GAMENET_TEST_ASSERT(observation.stopped);
    GAMENET_TEST_ASSERT(
        stopResult.outcome == gamenet::net::TcpServerStopOutcome::Drained);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    GAMENET_TEST_ASSERT(disconnects == 1);
    return observation;
}

EchoObservation runCompletionListenerReference() {
    gamenet::net::EventLoop loop;
    EchoObservation observation;
    std::optional<uring::IoUringTcpHubListenerStopSummary> listenerSummary;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubSummary;
    std::size_t closeCallbacks = 0;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubSummary = summary;
            observation.stopped = true;
            loop.quit();
        });
    hubPointer = &hub;

    auto endpoint = makeListener();
    const auto address = endpoint.address;
    auto listenOutcome = hub.listen(endpoint.socket.release(), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        return uring::IoUringTcpHubAcceptedConnectionCallbacks{
            .messageConsumer =
                [&](uring::IoUringTcpConnectionIdentity identity,
                    std::string_view payload) {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread());
                    observation.connected = true;
                    observation.payload.append(payload);
                    GAMENET_TEST_ASSERT(
                        hubPointer->send(identity, payload) ==
                        uring::IoUringTcpHubSendResult::Accepted);
                },
            .closeConsumer =
                [&](uring::IoUringTcpConnectionIdentity,
                    uring::IoUringTcpHubCloseReason) {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread());
                    ++closeCallbacks;
                },
        };
    });
    GAMENET_TEST_ASSERT(
        listenOutcome.result == uring::IoUringTcpHubListenResult::Accepted);
    GAMENET_TEST_ASSERT(listenOutcome.stopFuture.valid());
    auto replacementListener = makeListener();
    const int rejectedListenerDescriptor = replacementListener.socket.get();
    const auto duplicateListen = hub.listen(
        replacementListener.socket.release(),
        [] {
            return uring::IoUringTcpHubAcceptedConnectionCallbacks{
                .messageConsumer =
                    [](uring::IoUringTcpConnectionIdentity, std::string_view) {},
            };
        });
    GAMENET_TEST_ASSERT(
        duplicateListen.result ==
        uring::IoUringTcpHubListenResult::AlreadyListening);
    errno = 0;
    GAMENET_TEST_ASSERT(::fcntl(rejectedListenerDescriptor, F_GETFD) == -1);
    GAMENET_TEST_ASSERT(errno == EBADF);

    auto client = connectClient(address);
    constexpr std::string_view payload = "ioe-x9-echo";
    sendAll(client.get(), payload);
    std::string echoed;
    bool listenerStopRequested = false;
    bool hubStopRequested = false;
    const auto progressTimer = loop.runEvery(1ms, [&] {
        drainAvailable(client, echoed);
        if (echoed == payload && !listenerStopRequested) {
            client.close();
            listenerStopRequested = true;
            GAMENET_TEST_ASSERT(hub.stopListening());
        }
        if (!listenerStopRequested || hubStopRequested ||
            listenOutcome.stopFuture.wait_for(0ms) !=
                std::future_status::ready) {
            return;
        }
        listenerSummary = listenOutcome.stopFuture.get();
        hubStopRequested = true;
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(3s, [] {
        GAMENET_TEST_FAIL("completion listener reference timed out");
    });
    loop.loop();
    loop.cancel(progressTimer);

    GAMENET_TEST_ASSERT(observation.connected);
    GAMENET_TEST_ASSERT(observation.payload == payload);
    GAMENET_TEST_ASSERT(echoed == payload);
    GAMENET_TEST_ASSERT(observation.stopped);
    GAMENET_TEST_ASSERT(closeCallbacks == 1);
    GAMENET_TEST_ASSERT(listenerSummary.has_value());
    GAMENET_TEST_ASSERT(
        listenerSummary->reason ==
        uring::IoUringTcpHubListenerCloseReason::Explicit);
    GAMENET_TEST_ASSERT(listenerSummary->socketClosed);
    GAMENET_TEST_ASSERT(listenerSummary->acceptsRetired);
    GAMENET_TEST_ASSERT(listenerSummary->listener.acceptedSockets == 1);
    GAMENET_TEST_ASSERT(listenerSummary->listener.connectionsAdmitted == 1);
    GAMENET_TEST_ASSERT(listenerSummary->listener.activeAccepts == 0);
    GAMENET_TEST_ASSERT(hubSummary.has_value());
    GAMENET_TEST_ASSERT(hubSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubSummary->listener.has_value());
    GAMENET_TEST_ASSERT(hubSummary->hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.invariantFailures == 0);
    return observation;
}

void testProductionAndCompletionListenerEchoAndStop() {
    const auto production = runProductionEpollReference();
    const auto completion = runCompletionListenerReference();
    GAMENET_TEST_ASSERT(production.payload == completion.payload);
    GAMENET_TEST_ASSERT(production.connected == completion.connected);
    GAMENET_TEST_ASSERT(production.stopped == completion.stopped);
}

struct BurstClient {
    OwnedFd socket;
    std::string echoed;
};

void testListenerBurstCapacityRecoveryAndGenerationChurn() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpHubListenerStopSummary> listenerSummary;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubSummary;
    std::size_t closeCallbacks = 0;
    std::vector<uring::IoUringTcpConnectionIdentity> firstWaveIdentities;
    std::vector<uring::IoUringTcpConnectionIdentity> replacementIdentities;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(2, 8, 2),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubSummary = summary;
            loop.quit();
        });
    hubPointer = &hub;

    auto endpoint = makeListener();
    const auto address = endpoint.address;
    auto listenOutcome = hub.listen(endpoint.socket.release(), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        return uring::IoUringTcpHubAcceptedConnectionCallbacks{
            .messageConsumer =
                [&](uring::IoUringTcpConnectionIdentity identity,
                    std::string_view payload) {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread());
                    GAMENET_TEST_ASSERT(payload.size() == 1);
                    auto& identities = payload.front() <= 'c'
                        ? firstWaveIdentities
                        : replacementIdentities;
                    const auto duplicate = std::find(
                        identities.begin(), identities.end(), identity);
                    if (duplicate == identities.end()) {
                        identities.push_back(identity);
                    }
                    GAMENET_TEST_ASSERT(
                        hubPointer->send(identity, payload) ==
                        uring::IoUringTcpHubSendResult::Accepted);
                },
            .closeConsumer =
                [&](uring::IoUringTcpConnectionIdentity,
                    uring::IoUringTcpHubCloseReason) {
                    GAMENET_TEST_ASSERT(loop.isInLoopThread());
                    ++closeCallbacks;
                },
        };
    });
    GAMENET_TEST_ASSERT(
        listenOutcome.result == uring::IoUringTcpHubListenResult::Accepted);

    std::vector<BurstClient> firstWave;
    for (const char value : std::array{'a', 'b', 'c'}) {
        auto socket = connectClient(address);
        sendAll(socket.get(), std::string_view(&value, 1));
        firstWave.push_back({.socket = std::move(socket)});
    }
    std::vector<BurstClient> replacementWave;
    bool firstWaveClosed = false;
    bool replacementsConnected = false;
    bool listenerStopRequested = false;
    bool hubStopRequested = false;

    const auto progressTimer = loop.runEvery(1ms, [&] {
        for (auto& client : firstWave) {
            drainAvailable(client.socket, client.echoed);
        }
        for (auto& client : replacementWave) {
            drainAvailable(client.socket, client.echoed);
        }
        const auto firstEchoes = std::count_if(
            firstWave.begin(),
            firstWave.end(),
            [](const BurstClient& client) { return client.echoed.size() == 1; });
        const auto listenerMetrics = hub.listenerMetrics();
        if (!firstWaveClosed && firstEchoes == 2 &&
            listenerMetrics.connectionLimitRejections >= 1) {
            for (auto& client : firstWave) client.socket.close();
            firstWaveClosed = true;
        }
        if (firstWaveClosed && !replacementsConnected &&
            hub.metrics().activeConnections == 0) {
            for (const char value : std::array{'d', 'e'}) {
                auto socket = connectClient(address);
                sendAll(socket.get(), std::string_view(&value, 1));
                replacementWave.push_back({.socket = std::move(socket)});
            }
            replacementsConnected = true;
        }
        const auto replacementEchoes = std::count_if(
            replacementWave.begin(),
            replacementWave.end(),
            [](const BurstClient& client) { return client.echoed.size() == 1; });
        if (replacementsConnected && replacementEchoes == 2 &&
            !listenerStopRequested) {
            GAMENET_TEST_ASSERT(firstWaveIdentities.size() == 2);
            GAMENET_TEST_ASSERT(replacementIdentities.size() == 2);
            for (const auto oldIdentity : firstWaveIdentities) {
                GAMENET_TEST_ASSERT(
                    hub.send(oldIdentity, "stale") ==
                    uring::IoUringTcpHubSendResult::StaleConnection);
            }
            for (auto& client : replacementWave) client.socket.close();
            listenerStopRequested = true;
            GAMENET_TEST_ASSERT(hub.stopListening());
        }
        if (!listenerStopRequested || hubStopRequested ||
            listenOutcome.stopFuture.wait_for(0ms) !=
                std::future_status::ready) {
            return;
        }
        listenerSummary = listenOutcome.stopFuture.get();
        hubStopRequested = true;
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(4s, [] {
        GAMENET_TEST_FAIL("listener burst/capacity recovery timed out");
    });
    loop.loop();
    loop.cancel(progressTimer);

    GAMENET_TEST_ASSERT(listenerSummary.has_value());
    const auto& metrics = listenerSummary->listener;
    GAMENET_TEST_ASSERT(metrics.acceptSubmissions >= 7);
    GAMENET_TEST_ASSERT(metrics.acceptTerminals == metrics.acceptSubmissions);
    GAMENET_TEST_ASSERT(metrics.acceptedSockets == 5);
    GAMENET_TEST_ASSERT(metrics.connectionsAdmitted == 4);
    GAMENET_TEST_ASSERT(metrics.acceptedSocketRejections == 1);
    GAMENET_TEST_ASSERT(metrics.connectionLimitRejections == 1);
    GAMENET_TEST_ASSERT(metrics.maxActiveAccepts == 2);
    GAMENET_TEST_ASSERT(metrics.activeAccepts == 0);
    for (const auto replacement : replacementIdentities) {
        const auto reused = std::find_if(
            firstWaveIdentities.begin(),
            firstWaveIdentities.end(),
            [replacement](uring::IoUringTcpConnectionIdentity original) {
                return original.slot == replacement.slot &&
                    original.generation != replacement.generation;
            });
        GAMENET_TEST_ASSERT(reused != firstWaveIdentities.end());
    }
    GAMENET_TEST_ASSERT(closeCallbacks == 4);
    GAMENET_TEST_ASSERT(hubSummary.has_value());
    GAMENET_TEST_ASSERT(hubSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubSummary->hub.connectionsAccepted == 4);
    GAMENET_TEST_ASSERT(hubSummary->hub.connectionsRetired == 4);
    GAMENET_TEST_ASSERT(hubSummary->hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.invariantFailures == 0);
}

struct EstablishedPair {
    OwnedFd hub;
    OwnedFd peer;
};

EstablishedPair makeEstablishedPair() {
    auto endpoint = makeListener(1);
    auto peer = connectClient(endpoint.address);
    OwnedFd hub(::accept4(
        endpoint.socket.get(),
        nullptr,
        nullptr,
        SOCK_NONBLOCK | SOCK_CLOEXEC));
    GAMENET_TEST_ASSERT(hub.valid());
    return {.hub = std::move(hub), .peer = std::move(peer)};
}

void testListenerFirstArmOperationPressureIsTyped() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubSummary;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(2, 4, 1),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubSummary = summary;
            loop.quit();
        });
    OwnedFd nonListener(::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_TCP));
    GAMENET_TEST_ASSERT(nonListener.valid());
    const int invalidDescriptor = nonListener.get();
    const auto invalidListen = hub.listen(
        nonListener.release(),
        [] {
            return uring::IoUringTcpHubAcceptedConnectionCallbacks{
                .messageConsumer =
                    [](uring::IoUringTcpConnectionIdentity, std::string_view) {},
            };
        });
    GAMENET_TEST_ASSERT(
        invalidListen.result ==
        uring::IoUringTcpHubListenResult::RejectedInvalid);
    GAMENET_TEST_ASSERT(!invalidListen.stopFuture.valid());
    errno = 0;
    GAMENET_TEST_ASSERT(::fcntl(invalidDescriptor, F_GETFD) == -1);
    GAMENET_TEST_ASSERT(errno == EBADF);
    auto first = makeEstablishedPair();
    auto second = makeEstablishedPair();
    const auto addedFirst = hub.addConnection(
        first.hub.release(),
        [](uring::IoUringTcpConnectionIdentity, std::string_view) {});
    const auto addedSecond = hub.addConnection(
        second.hub.release(),
        [](uring::IoUringTcpConnectionIdentity, std::string_view) {});
    GAMENET_TEST_ASSERT(
        addedFirst.result == uring::IoUringTcpHubAddResult::Accepted);
    GAMENET_TEST_ASSERT(
        addedSecond.result == uring::IoUringTcpHubAddResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(addedFirst.identity, "x") ==
        uring::IoUringTcpHubSendResult::Accepted);
    GAMENET_TEST_ASSERT(
        hub.send(addedSecond.identity, "y") ==
        uring::IoUringTcpHubSendResult::Accepted);

    auto endpoint = makeListener();
    const int transferredDescriptor = endpoint.socket.get();
    const auto listenOutcome = hub.listen(
        endpoint.socket.release(),
        [] {
            return uring::IoUringTcpHubAcceptedConnectionCallbacks{
                .messageConsumer =
                    [](uring::IoUringTcpConnectionIdentity, std::string_view) {},
            };
        });
    GAMENET_TEST_ASSERT(
        listenOutcome.result ==
        uring::IoUringTcpHubListenResult::EngineRejected);
    GAMENET_TEST_ASSERT(listenOutcome.stopFuture.valid());
    GAMENET_TEST_ASSERT(
        listenOutcome.stopFuture.wait_for(0ms) == std::future_status::ready);
    const auto listenerSummary = listenOutcome.stopFuture.get();
    GAMENET_TEST_ASSERT(
        listenerSummary.reason ==
        uring::IoUringTcpHubListenerCloseReason::EngineRejected);
    GAMENET_TEST_ASSERT(listenerSummary.socketClosed);
    GAMENET_TEST_ASSERT(listenerSummary.acceptsRetired);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptSubmissions == 0);
    GAMENET_TEST_ASSERT(listenerSummary.listener.engineRejections == 1);
    GAMENET_TEST_ASSERT(
        listenerSummary.listener.listenerSocketCloseCount == 1);
    errno = 0;
    GAMENET_TEST_ASSERT(::fcntl(transferredDescriptor, F_GETFD) == -1);
    GAMENET_TEST_ASSERT(errno == EBADF);

    std::string firstPeerOutput;
    std::string secondPeerOutput;
    bool hubStopRequested = false;
    const auto progressTimer = loop.runEvery(1ms, [&] {
        drainAvailable(first.peer, firstPeerOutput);
        drainAvailable(second.peer, secondPeerOutput);
        if (hubStopRequested || firstPeerOutput != "x" ||
            secondPeerOutput != "y") {
            return;
        }
        GAMENET_TEST_ASSERT(
            hub.phase() == uring::IoUringTcpHubPhase::Running);
        GAMENET_TEST_ASSERT(hub.metrics().activeConnections == 2);
        hubStopRequested = true;
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(3s, [] {
        GAMENET_TEST_FAIL("operation-pressure Hub stop timed out");
    });
    loop.loop();
    loop.cancel(progressTimer);
    GAMENET_TEST_ASSERT(firstPeerOutput == "x");
    GAMENET_TEST_ASSERT(secondPeerOutput == "y");
    GAMENET_TEST_ASSERT(hubSummary.has_value());
    GAMENET_TEST_ASSERT(hubSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubSummary->listener.has_value());
    GAMENET_TEST_ASSERT(hubSummary->hub.activeConnections == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.invariantFailures == 0);
}

void testListenerFactoryReentryStopsAdmission() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubSummary;
    std::size_t factoryCalls = 0;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubSummary = summary;
            loop.quit();
        });
    hubPointer = &hub;
    auto endpoint = makeListener();
    const auto address = endpoint.address;
    const auto listenOutcome = hub.listen(endpoint.socket.release(), [&] {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());
        ++factoryCalls;
        GAMENET_TEST_ASSERT(hubPointer->stopListening());
        return uring::IoUringTcpHubAcceptedConnectionCallbacks{
            .messageConsumer =
                [](uring::IoUringTcpConnectionIdentity, std::string_view) {},
        };
    });
    GAMENET_TEST_ASSERT(
        listenOutcome.result == uring::IoUringTcpHubListenResult::Accepted);
    auto client = connectClient(address);
    bool hubStopRequested = false;
    const auto progressTimer = loop.runEvery(1ms, [&] {
        if (hubStopRequested ||
            listenOutcome.stopFuture.wait_for(0ms) !=
                std::future_status::ready) {
            return;
        }
        hubStopRequested = true;
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(3s, [] {
        GAMENET_TEST_FAIL("listener factory re-entry timed out");
    });
    loop.loop();
    loop.cancel(progressTimer);

    const auto listenerSummary = listenOutcome.stopFuture.get();
    GAMENET_TEST_ASSERT(factoryCalls == 1);
    GAMENET_TEST_ASSERT(
        listenerSummary.reason ==
        uring::IoUringTcpHubListenerCloseReason::Explicit);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptSubmissions == 2);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptTerminals == 2);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptedSockets == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.connectionsAdmitted == 0);
    GAMENET_TEST_ASSERT(
        listenerSummary.listener.acceptedSocketRejections == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptCancellations == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.activeAccepts == 0);
    GAMENET_TEST_ASSERT(hubSummary.has_value());
    GAMENET_TEST_ASSERT(hubSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubSummary->hub.connectionsAccepted == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.invariantFailures == 0);
}

void runListenerFactoryFailure(bool throws) {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> hubSummary;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(2, 8, 1),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            hubSummary = summary;
            loop.quit();
        });
    auto endpoint = makeListener();
    const auto address = endpoint.address;
    const auto listenOutcome = hub.listen(
        endpoint.socket.release(),
        [throws]() -> uring::IoUringTcpHubAcceptedConnectionCallbacks {
            if (throws) throw std::runtime_error("factory failure");
            return {};
        });
    GAMENET_TEST_ASSERT(
        listenOutcome.result == uring::IoUringTcpHubListenResult::Accepted);
    auto client = connectClient(address);
    bool hubStopRequested = false;
    const auto progressTimer = loop.runEvery(1ms, [&] {
        if (hubStopRequested ||
            listenOutcome.stopFuture.wait_for(0ms) !=
                std::future_status::ready) {
            return;
        }
        hubStopRequested = true;
        GAMENET_TEST_ASSERT(hub.stop());
    });
    loop.runAfter(3s, [] {
        GAMENET_TEST_FAIL("listener factory failure timed out");
    });
    loop.loop();
    loop.cancel(progressTimer);

    const auto listenerSummary = listenOutcome.stopFuture.get();
    GAMENET_TEST_ASSERT(
        listenerSummary.reason ==
        uring::IoUringTcpHubListenerCloseReason::CallbackFailed);
    GAMENET_TEST_ASSERT(listenerSummary.socketClosed);
    GAMENET_TEST_ASSERT(listenerSummary.acceptsRetired);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptSubmissions == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptTerminals == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.acceptedSockets == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.connectionsAdmitted == 0);
    GAMENET_TEST_ASSERT(
        listenerSummary.listener.acceptedSocketRejections == 1);
    GAMENET_TEST_ASSERT(listenerSummary.listener.callbackFailures == 1);
    GAMENET_TEST_ASSERT(hubSummary.has_value());
    GAMENET_TEST_ASSERT(hubSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(hubSummary->hub.connectionsAccepted == 0);
    GAMENET_TEST_ASSERT(hubSummary->hub.invariantFailures == 0);
}

void testListenerFactoryFailuresFailClosed() {
    runListenerFactoryFailure(false);
    runListenerFactoryFailure(true);
}

void testListenerOwnerQuitDrains() {
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> stoppedSummary;
    uring::IoUringTcpConnectionHub hub(
        &loop,
        listenerOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            stoppedSummary = summary;
        });
    auto endpoint = makeListener();
    const auto listenOutcome = hub.listen(
        endpoint.socket.release(),
        [] {
            return uring::IoUringTcpHubAcceptedConnectionCallbacks{
                .messageConsumer =
                    [](uring::IoUringTcpConnectionIdentity, std::string_view) {},
            };
        });
    GAMENET_TEST_ASSERT(
        listenOutcome.result == uring::IoUringTcpHubListenResult::Accepted);
    loop.runAfter(1ms, [&] { loop.quit(); });
    loop.loop();

    GAMENET_TEST_ASSERT(stoppedSummary.has_value());
    GAMENET_TEST_ASSERT(stoppedSummary->allConnectionsStopped);
    GAMENET_TEST_ASSERT(stoppedSummary->listener.has_value());
    GAMENET_TEST_ASSERT(
        stoppedSummary->listener->reason ==
        uring::IoUringTcpHubListenerCloseReason::EventLoopQuiescing);
    GAMENET_TEST_ASSERT(stoppedSummary->listener->socketClosed);
    GAMENET_TEST_ASSERT(stoppedSummary->listener->acceptsRetired);
    GAMENET_TEST_ASSERT(
        stoppedSummary->listener->listener.acceptSubmissions == 2);
    GAMENET_TEST_ASSERT(
        stoppedSummary->listener->listener.acceptTerminals == 2);
    GAMENET_TEST_ASSERT(
        stoppedSummary->listener->listener.acceptCancellations == 2);
    GAMENET_TEST_ASSERT(
        stoppedSummary->listener->listener.activeAccepts == 0);
    GAMENET_TEST_ASSERT(stoppedSummary->hub.activeOperationRoutes == 0);
    GAMENET_TEST_ASSERT(stoppedSummary->hub.invariantFailures == 0);
    GAMENET_TEST_ASSERT(
        hub.stopFuture().wait_for(0ms) == std::future_status::ready);
}

}  // namespace

int main() {
    testProductionAndCompletionListenerEchoAndStop();
    testListenerBurstCapacityRecoveryAndGenerationChurn();
    testListenerFirstArmOperationPressureIsTyped();
    testListenerFactoryReentryStopsAdmission();
    testListenerFactoryFailuresFailClosed();
    testListenerOwnerQuitDrains();
    return 0;
}
