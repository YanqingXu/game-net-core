#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/game_session/PlayerSession.h"
#include "gamenet/transport/TcpTransportEndpoint.h"
#include "support/ClientSocket.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

void verifyReconnectAndMultiLoopDelivery() {
    using namespace std::chrono_literals;
    constexpr int reconnectCycles = 8;

    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop, gamenet::net::InetAddress(0, true), "broadcast-real-multi-loop");
    server.setThreadNum(2);

    std::atomic<std::uint64_t> nextTransportId{1};
    std::unordered_map<
        std::string,
        std::shared_ptr<gamenet::transport::TcpTransportEndpoint>>
        activeEndpoints;
    std::vector<std::unique_ptr<gamenet::net::TcpClient>> clients;
    int cycle = 0;
    int receivedThisCycle = 0;
    int totalReceived = 0;
    bool broadcastScheduled = false;
    bool waitingForDisconnect = false;

    std::function<void()> startCycle;
    std::function<void()> advanceWhenDisconnected;

    advanceWhenDisconnected = [&] {
        loop.assertInLoopThread();
        if (!waitingForDisconnect || !activeEndpoints.empty()) return;

        waitingForDisconnect = false;
        broadcastScheduled = false;
        receivedThisCycle = 0;
        if (cycle == reconnectCycles) {
            server.stop();
            loop.quit();
            return;
        }
        startCycle();
    };

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        const auto connectionName = connection->name();
        if (!connection->connected()) {
            loop.queueInLoop([&, connectionName] {
                activeEndpoints.erase(connectionName);
                advanceWhenDisconnected();
            });
            return;
        }

        auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
            gamenet::transport::TransportSessionId{nextTransportId.fetch_add(1)}, connection);
        loop.queueInLoop([&, connectionName, endpoint = std::move(endpoint)] {
            activeEndpoints.emplace(connectionName, endpoint);
            if (activeEndpoints.size() != 2 || broadcastScheduled) return;

            broadcastScheduled = true;
            std::vector<std::shared_ptr<gamenet::transport::TcpTransportEndpoint>> endpoints;
            endpoints.reserve(activeEndpoints.size());
            for (const auto& [name, activeEndpoint] : activeEndpoints) {
                (void)name;
                endpoints.push_back(activeEndpoint);
            }
            GAMENET_TEST_ASSERT(
                endpoints[0]->ownerExecutor().id() != endpoints[1]->ownerExecutor().id());

            std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> sessions;
            sessions.reserve(endpoints.size());
            for (std::size_t index = 0; index < endpoints.size(); ++index) {
                auto session = std::make_shared<gamenet::game_session::PlayerSession>(
                    static_cast<gamenet::game_session::SessionId>(
                        (cycle - 1) * 2 + static_cast<int>(index) + 1),
                    "tcp-broadcast-" + std::to_string(cycle) + "-" + std::to_string(index),
                    endpoints[index],
                    gamenet::game_session::PlayerSession::Clock::now());
                session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
                sessions.push_back(std::move(session));
            }

            const auto payload =
                std::make_shared<const std::string>("real-broadcast-" + std::to_string(cycle));
            gamenet::broadcast::BroadcastRouter router(&loop);
            gamenet::broadcast::BroadcastDispatcher dispatcher(
                {.maxEndpointsPerTask = 1, .maxBytesPerTask = 64});
            auto plan = router.route(payload, sessions);
            const auto summary = dispatcher.dispatch(std::move(plan));
            GAMENET_TEST_ASSERT(summary.scheduledEndpoints == 2);
            GAMENET_TEST_ASSERT(summary.scheduledTasks == 2);
        });
    });

    startCycle = [&] {
        loop.assertInLoopThread();
        ++cycle;
        GAMENET_TEST_ASSERT(cycle <= reconnectCycles);
        const auto expectedCycle = cycle;
        const auto expectedPayload = "real-broadcast-" + std::to_string(expectedCycle);

        for (int clientIndex = 0; clientIndex < 2; ++clientIndex) {
            auto client = std::make_unique<gamenet::net::TcpClient>(
                &loop,
                server.listenAddress(),
                "broadcast-reconnect-client-" + std::to_string(expectedCycle) + "-" +
                    std::to_string(clientIndex));
            auto receivedBytes = std::make_shared<std::string>();
            auto completed = std::make_shared<bool>(false);
            client->setMessageCallback(
                [&, expectedCycle, expectedPayload, receivedBytes, completed](
                    const gamenet::net::TcpConnectionPtr& connection,
                    gamenet::net::Buffer* buffer) {
                    receivedBytes->append(buffer->retrieveAllAsString());
                    if (receivedBytes->size() < expectedPayload.size()) return;
                    GAMENET_TEST_ASSERT(!*completed);
                    GAMENET_TEST_ASSERT(*receivedBytes == expectedPayload);
                    GAMENET_TEST_ASSERT(cycle == expectedCycle);
                    *completed = true;
                    connection->forceClose();

                    ++receivedThisCycle;
                    ++totalReceived;
                    if (receivedThisCycle == 2) {
                        waitingForDisconnect = true;
                        advanceWhenDisconnected();
                    }
                });
            client->connect();
            clients.push_back(std::move(client));
        }
    };

    server.start();
    startCycle();
    loop.runAfter(10s, [&] { GAMENET_TEST_FAIL("real multi-loop reconnect broadcast timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT(cycle == reconnectCycles);
    GAMENET_TEST_ASSERT(totalReceived == reconnectCycles * 2);
    GAMENET_TEST_ASSERT(activeEndpoints.empty());
}

void verifyNonReadingPeerDoesNotBlockAnotherOwnerLoop() {
    using namespace std::chrono_literals;
    constexpr std::size_t messageCount = 16;
    constexpr std::size_t messageBytes = 256 * 1024;
    constexpr std::size_t highWaterMark = 64 * 1024;

    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop, gamenet::net::InetAddress(0, true), "broadcast-real-slow-peer");
    server.setThreadNum(2);

    std::atomic<std::uint64_t> nextTransportId{1};
    gamenet::net::SocketFd slowClientFd = gamenet::net::kInvalidSocket;
    std::unique_ptr<gamenet::net::TcpClient> normalClient;
    std::shared_ptr<gamenet::transport::TcpTransportEndpoint> slowEndpoint;
    std::shared_ptr<gamenet::transport::TcpTransportEndpoint> normalEndpoint;
    std::string slowConnectionName;
    std::string expectedBytes;
    std::string receivedBytes;
    bool broadcastScheduled = false;
    bool slowHighWaterObserved = false;
    bool normalComplete = false;
    bool stopping = false;
    bool slowWasOpenWhenNormalCompleted = false;
    int disconnectedConnections = 0;

    std::function<void()> finishWhenProven;
    std::function<void()> stopWhenDisconnected;
    stopWhenDisconnected = [&] {
        loop.assertInLoopThread();
        if (!stopping || disconnectedConnections != 2) return;
        if (server.connectionCount() != 0) {
            loop.runAfter(1ms, stopWhenDisconnected);
            return;
        }
        server.stop();
        loop.quit();
    };

    finishWhenProven = [&] {
        loop.assertInLoopThread();
        if (stopping || !normalComplete || !slowHighWaterObserved) return;

        GAMENET_TEST_ASSERT(slowEndpoint);
        GAMENET_TEST_ASSERT(slowEndpoint->isOpen());
        slowWasOpenWhenNormalCompleted = true;
        stopping = true;
        if (normalClient) normalClient->disconnect();
        gamenet::test::closeTestSocket(slowClientFd);
        slowClientFd = gamenet::net::kInvalidSocket;
    };

    server.setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection, std::size_t bytes) {
            const auto connectionName = connection->name();
            loop.queueInLoop([&, connectionName, bytes] {
                if (connectionName != slowConnectionName) return;
                GAMENET_TEST_ASSERT(bytes >= highWaterMark);
                slowHighWaterObserved = true;
                finishWhenProven();
            });
        },
        highWaterMark);

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) {
            loop.queueInLoop([&] {
                ++disconnectedConnections;
                GAMENET_TEST_ASSERT(disconnectedConnections <= 2);
                stopWhenDisconnected();
            });
            return;
        }

        const auto connectionName = connection->name();
        auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
            gamenet::transport::TransportSessionId{nextTransportId.fetch_add(1)}, connection);
        loop.queueInLoop([&, connectionName, endpoint = std::move(endpoint)] {
            if (!slowEndpoint) {
                slowConnectionName = connectionName;
                slowEndpoint = endpoint;
                normalClient = std::make_unique<gamenet::net::TcpClient>(
                    &loop, server.listenAddress(), "broadcast-normal-reader");
                normalClient->setMessageCallback(
                    [&](const gamenet::net::TcpConnectionPtr&,
                        gamenet::net::Buffer* buffer) {
                        receivedBytes.append(buffer->retrieveAllAsString());
                        if (receivedBytes.size() < expectedBytes.size()) return;
                        GAMENET_TEST_ASSERT(!normalComplete);
                        GAMENET_TEST_ASSERT(receivedBytes == expectedBytes);
                        normalComplete = true;
                        finishWhenProven();
                    });
                normalClient->connect();
                return;
            }

            GAMENET_TEST_ASSERT(!normalEndpoint);
            normalEndpoint = endpoint;
            GAMENET_TEST_ASSERT(
                slowEndpoint->ownerExecutor().id() != normalEndpoint->ownerExecutor().id());

            std::vector<std::shared_ptr<const gamenet::game_session::PlayerSession>> sessions;
            sessions.reserve(2);
            for (const auto& activeEndpoint : {slowEndpoint, normalEndpoint}) {
                auto session = std::make_shared<gamenet::game_session::PlayerSession>(
                    activeEndpoint->id().value,
                    "real-slow-peer-" + std::to_string(activeEndpoint->id().value),
                    activeEndpoint,
                    gamenet::game_session::PlayerSession::Clock::now());
                session->markOnline(gamenet::game_session::PlayerSession::Clock::now());
                sessions.push_back(std::move(session));
            }

            gamenet::broadcast::BroadcastRouter router(
                &loop,
                {.softFanout = 2,
                 .hardFanout = 2,
                 .softBytes = messageBytes * 2,
                 .hardBytes = messageBytes * 2});
            gamenet::broadcast::BroadcastDispatcher dispatcher(
                {.maxEndpointsPerTask = 1, .maxBytesPerTask = messageBytes});
            expectedBytes.reserve(messageCount * messageBytes);
            for (std::size_t sequence = 0; sequence < messageCount; ++sequence) {
                const auto prefix = "sequence-" + std::to_string(sequence) + ":";
                std::string bytes = prefix;
                bytes.append(messageBytes - prefix.size(), static_cast<char>('a' + sequence));
                expectedBytes.append(bytes);
                auto plan = router.route(
                    std::make_shared<const std::string>(std::move(bytes)), sessions);
                const auto summary = dispatcher.dispatch(std::move(plan));
                GAMENET_TEST_ASSERT(summary.scheduledEndpoints == 2);
                GAMENET_TEST_ASSERT(summary.scheduledTasks == 2);
            }
            broadcastScheduled = true;
        });
    });

    server.start();
    slowClientFd =
        gamenet::test::connectTestClientWithReceiveBuffer(server.listenAddress(), 4096);
    loop.runAfter(10s, [&] {
        GAMENET_TEST_FAIL("real slow-peer broadcast isolation timed out");
    });
    loop.loop();

    if (gamenet::net::sockets::isValid(slowClientFd)) {
        gamenet::test::closeTestSocket(slowClientFd);
    }
    GAMENET_TEST_ASSERT(broadcastScheduled);
    GAMENET_TEST_ASSERT(slowHighWaterObserved);
    GAMENET_TEST_ASSERT(normalComplete);
    GAMENET_TEST_ASSERT(slowWasOpenWhenNormalCompleted);
    GAMENET_TEST_ASSERT(disconnectedConnections == 2);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    GAMENET_TEST_ASSERT(receivedBytes == expectedBytes);
}

int main() {
    verifyReconnectAndMultiLoopDelivery();
    verifyNonReadingPeerDoesNotBlockAnotherOwnerLoop();
}
