#include "gamenet/core/net/Acceptor.h"
#include "gamenet/core/net/Connector.h"
#include "gamenet/core/net/ConnectorOptions.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopExecutor.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/PostResult.h"
#include "gamenet/core/net/Socket.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include "support/ThreadHandoff.h"

#include "../../../src/core/net/detail/EventLoopIocpAssociationHarness.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) {
        throw std::runtime_error("copy rejected");
    }
    ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
    void operator()() const {}
};

void testTypedExecutorAdmission() {
    gamenet::net::EventLoopExecutor expired;
    {
        gamenet::net::EventLoopOptions options;
        options.maxPendingFunctors = 1;
        options.reservedPendingFunctors = 0;
        options.maxFunctorsPerIteration = 1;
        gamenet::net::EventLoop loop(options);
        const auto executor = loop.executor();
        expired = executor;

        bool ran = false;
        gamenet::net::PostResult postAfterQuit =
            gamenet::net::PostResult::Accepted;
        GAMENET_TEST_ASSERT(
            executor.post([&] {
                ran = true;
                loop.quit();
                postAfterQuit = executor.post([] {});
            }) == gamenet::net::PostResult::Accepted);
        GAMENET_TEST_ASSERT(
            executor.post([] {}) == gamenet::net::PostResult::QueueFull);

        ThrowOnCopy throwing;
        bool escaped = false;
        gamenet::net::PostResult throwingResult =
            gamenet::net::PostResult::Accepted;
        try {
            throwingResult = executor.post(throwing);
        } catch (...) {
            escaped = true;
        }
        GAMENET_TEST_ASSERT(!escaped);
        GAMENET_TEST_ASSERT(
            throwingResult == gamenet::net::PostResult::QueueFull);

        loop.loop();
        GAMENET_TEST_ASSERT(ran);
        GAMENET_TEST_ASSERT(
            postAfterQuit == gamenet::net::PostResult::Shutdown);
        GAMENET_TEST_ASSERT(
            executor.post([] {}) == gamenet::net::PostResult::Shutdown);
    }

    GAMENET_TEST_ASSERT(
        expired.post([] {}) ==
        gamenet::net::PostResult::OwnerUnavailable);
}

void testConnectorOwnerOnlyBoundary() {
    gamenet::net::EventLoop loop;
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        gamenet::net::InetAddress("127.0.0.1", 19999));

    std::atomic<int> rejected{0};
    gamenet::test::runFromNonOwnerThread([&] {
        auto expectWrongThread = [&](auto&& operation) {
            try {
                operation();
            } catch (const std::runtime_error&) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        };

        expectWrongThread([&] { connector->start(); });
        expectWrongThread([&] { connector->stop(); });
        expectWrongThread([&] { connector->restart(); });
        expectWrongThread([&] { connector->setRetryDelay(20ms, 100ms); });
        expectWrongThread([&] { connector->setRetryEnabled(true); });

        for (int index = 0; index < 1000; ++index) {
            GAMENET_TEST_ASSERT(
                connector->state() ==
                gamenet::net::Connector::kDisconnected);
        }
    });

    GAMENET_TEST_ASSERT(rejected.load(std::memory_order_relaxed) == 5);
    GAMENET_TEST_ASSERT(
        connector->state() == gamenet::net::Connector::kDisconnected);
    GAMENET_TEST_ASSERT(!connector->retryEnabled());
}

void testConnectSuccessObserverMayStopReentrantly() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        acceptor.listenAddress());

    int successEvents = 0;
    int publishedConnections = 0;
    acceptor.setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd,
            const gamenet::net::InetAddress&) {
            gamenet::net::sockets::close(sockfd);
        });
    connector->setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd) {
            ++publishedConnections;
            gamenet::net::sockets::close(sockfd);
        });
    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            if (event != gamenet::net::ConnectorEvent::ConnectSuccess) {
                return;
            }
            ++successEvents;
            connector->stop();
            loop.runAfter(30ms, [&] { loop.quit(); });
        });

    acceptor.listen();
    connector->start();
    bool lateRetryDelayRejected = false;
    try {
        connector->setRetryDelay(20ms, 100ms);
    } catch (const std::logic_error&) {
        lateRetryDelayRejected = true;
    }
    GAMENET_TEST_ASSERT(lateRetryDelayRejected);
    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "re-entrant Connector stop did not settle");

    GAMENET_TEST_ASSERT(successEvents == 1);
    GAMENET_TEST_ASSERT(publishedConnections == 0);
    GAMENET_TEST_ASSERT(
        connector->state() == gamenet::net::Connector::kDisconnected);
    acceptor.stop();
}

void testConnectSuccessObserverMayRestartReentrantly() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        acceptor.listenAddress());

    int attempts = 0;
    int successEvents = 0;
    int publishedConnections = 0;
    acceptor.setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd,
            const gamenet::net::InetAddress&) {
            gamenet::net::sockets::close(sockfd);
        });
    connector->setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd) {
            ++publishedConnections;
            gamenet::net::sockets::close(sockfd);
        });
    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            if (event == gamenet::net::ConnectorEvent::ConnectAttempt) {
                ++attempts;
                return;
            }
            if (event != gamenet::net::ConnectorEvent::ConnectSuccess) {
                return;
            }

            ++successEvents;
            if (successEvents == 1) {
                connector->restart();
                return;
            }

            connector->stop();
            loop.runAfter(30ms, [&] { loop.quit(); });
        });

    acceptor.listen();
    connector->start();
    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "re-entrant Connector restart lost the newer generation");

    GAMENET_TEST_ASSERT(attempts == 2);
    GAMENET_TEST_ASSERT(successEvents == 2);
    GAMENET_TEST_ASSERT(publishedConnections == 0);
    GAMENET_TEST_ASSERT(
        connector->state() == gamenet::net::Connector::kDisconnected);
    acceptor.stop();
}

void testConnectSuccessObserverMayStopThenStartReentrantly() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        acceptor.listenAddress());

    int attempts = 0;
    int successEvents = 0;
    int publishedConnections = 0;
    acceptor.setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd,
            const gamenet::net::InetAddress&) {
            gamenet::net::sockets::close(sockfd);
        });
    connector->setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd) {
            ++publishedConnections;
            gamenet::net::sockets::close(sockfd);
        });
    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            if (event == gamenet::net::ConnectorEvent::ConnectAttempt) {
                ++attempts;
                return;
            }
            if (event != gamenet::net::ConnectorEvent::ConnectSuccess) {
                return;
            }

            ++successEvents;
            connector->stop();
            if (successEvents == 1) {
                connector->start();
                return;
            }
            loop.runAfter(30ms, [&] { loop.quit(); });
        });

    acceptor.listen();
    connector->start();
    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "re-entrant Connector stop/start lost the latest request");

    GAMENET_TEST_ASSERT(attempts == 2);
    GAMENET_TEST_ASSERT(successEvents == 2);
    GAMENET_TEST_ASSERT(publishedConnections == 0);
    GAMENET_TEST_ASSERT(
        connector->state() == gamenet::net::Connector::kDisconnected);
    acceptor.stop();
}

void testThrowingNewConnectionCallbackClosesUnpublishedFd() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor acceptor(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        acceptor.listenAddress());

    int callbackCalls = 0;
    bool staleAssociation = false;
    gamenet::net::SocketFd transferredSocket =
        gamenet::net::kInvalidSocket;
    acceptor.setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd,
            const gamenet::net::InetAddress&) {
            gamenet::net::sockets::close(sockfd);
        });
    connector->setNewConnectionCallback(
        [&](gamenet::net::SocketFd sockfd) {
            gamenet::net::Socket ownedSocket(sockfd);
            transferredSocket = sockfd;
            ++callbackCalls;
            loop.queueInLoop([&] {
                staleAssociation =
                    gamenet::net::detail::
                        EventLoopIocpAssociationHarness::tracks(
                            loop,
                            transferredSocket);
                loop.quit();
            });
            throw std::runtime_error("handoff rejected");
        });

    acceptor.listen();
    connector->start();
    gamenet::test::runLoopWithTimeout(
        loop,
        1s,
        "throwing Connector handoff callback did not settle");

    GAMENET_TEST_ASSERT(callbackCalls == 1);
    GAMENET_TEST_ASSERT(!staleAssociation);
    GAMENET_TEST_ASSERT(
        connector->state() == gamenet::net::Connector::kDisconnected);
    acceptor.stop();
}

void testRestartRestoresConfiguredInitialDelayAndRejectsStaleRetry() {
    gamenet::net::EventLoop loop;
    gamenet::net::Acceptor unavailable(
        &loop,
        gamenet::net::InetAddress(0, true),
        true);

    gamenet::net::ConnectorOptions options;
    options.enableRetry = true;
    options.initRetryDelay = 60ms;
    options.maxRetryDelay = 240ms;
    options.connectTimeout = 100ms;
    auto connector = std::make_shared<gamenet::net::Connector>(
        &loop,
        unavailable.listenAddress(),
        options);

    std::vector<std::chrono::steady_clock::time_point> attempts;
    int retryScheduled = 0;
    bool restartQueued = false;
    bool stopQueued = false;
    std::function<void()> waitForStopped;

    waitForStopped = [&] {
        if (connector->state() != gamenet::net::Connector::kDisconnected) {
            loop.runAfter(10ms, waitForStopped);
            return;
        }

        loop.runAfter(170ms, [&] {
            GAMENET_TEST_ASSERT(attempts.size() == 4);
            loop.quit();
        });
    };

    connector->setConnectorEventCallback(
        [&](const gamenet::net::InetAddress&, gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (event == gamenet::net::ConnectorEvent::ConnectAttempt) {
                attempts.push_back(std::chrono::steady_clock::now());
                if (attempts.size() == 4 && !stopQueued) {
                    stopQueued = true;
                    loop.queueInLoop([&] {
                        connector->stop();
                        loop.runAfter(10ms, waitForStopped);
                    });
                }
                return;
            }

            if (event == gamenet::net::ConnectorEvent::RetryScheduled) {
                ++retryScheduled;
                if (retryScheduled == 2 && !restartQueued) {
                    restartQueued = true;
                    loop.queueInLoop([&] {
                        connector->stop();
                        connector->restart();
                    });
                }
            }
        });

    connector->start();
    loop.runAfter(2s, [&] {
        GAMENET_TEST_FAIL(
            "timed out waiting for configured Connector restart backoff");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(restartQueued);
    GAMENET_TEST_ASSERT(stopQueued);
    GAMENET_TEST_ASSERT(attempts.size() == 4);
    const auto restartedDelay =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            attempts[3] - attempts[2]);
    GAMENET_TEST_ASSERT(restartedDelay >= 25ms);
    GAMENET_TEST_ASSERT(restartedDelay < 250ms);
}

void testRejectedStopDoesNotSupersedeAcceptedConnect() {
    gamenet::net::EventLoopOptions loopOptions;
    loopOptions.maxPendingFunctors = 1;
    loopOptions.reservedPendingFunctors = 0;
    loopOptions.maxFunctorsPerIteration = 1;
    gamenet::net::EventLoop loop(loopOptions);
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-admission-connect-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-admission-connect-client");

    std::atomic<gamenet::net::PostResult> connectResult{
        gamenet::net::PostResult::OwnerUnavailable};
    std::atomic<gamenet::net::PostResult> stopResult{
        gamenet::net::PostResult::Accepted};
    int clientConnected = 0;
    int clientDisconnected = 0;
    int serverConnected = 0;
    int serverDisconnected = 0;

    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (!connection->connected()) {
                ++serverDisconnected;
                if (clientDisconnected == 1) {
                    server.stop();
                    loop.quit();
                }
            }
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                ++clientConnected;
                connection->forceClose();
                return;
            }
            ++clientDisconnected;
            if (serverDisconnected == 1) {
                server.stop();
                loop.quit();
            }
        });

    server.start();
    gamenet::test::runFromNonOwnerThread([&] {
        connectResult.store(client.tryConnect(), std::memory_order_relaxed);
        stopResult.store(client.tryStop(), std::memory_order_relaxed);
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        2s,
        "accepted connect was superseded by rejected stop");

    GAMENET_TEST_ASSERT(
        connectResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        stopResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::QueueFull);
    GAMENET_TEST_ASSERT(clientConnected == 1);
    GAMENET_TEST_ASSERT(clientDisconnected == 1);
    GAMENET_TEST_ASSERT(serverDisconnected == 1);
}

void testRejectedConnectDoesNotSupersedeAcceptedDisconnect() {
    gamenet::net::EventLoopOptions loopOptions;
    loopOptions.maxPendingFunctors = 1;
    loopOptions.reservedPendingFunctors = 0;
    loopOptions.maxFunctorsPerIteration = 1;
    gamenet::net::EventLoop loop(loopOptions);
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-admission-disconnect-server");
    gamenet::net::ConnectorOptions connectorOptions;
    connectorOptions.enableRetry = true;
    connectorOptions.initRetryDelay = 300ms;
    connectorOptions.maxRetryDelay = 300ms;
    connectorOptions.connectTimeout = 50ms;
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-admission-disconnect-client",
        connectorOptions);

    std::atomic<gamenet::net::PostResult> disconnectResult{
        gamenet::net::PostResult::OwnerUnavailable};
    std::atomic<gamenet::net::PostResult> reconnectResult{
        gamenet::net::PostResult::Accepted};
    int serverConnected = 0;
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++serverConnected;
            }
        });

    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);
    std::function<void()> issueRequests;
    issueRequests = [&] {
        if (loop.pendingFunctorCount() != 0) {
            loop.runAfter(10ms, issueRequests);
            return;
        }
        gamenet::test::runFromNonOwnerThread([&] {
            disconnectResult.store(
                client.tryDisconnect(),
                std::memory_order_relaxed);
            reconnectResult.store(
                client.tryConnect(),
                std::memory_order_relaxed);
        });
        GAMENET_TEST_ASSERT(
            disconnectResult.load(std::memory_order_relaxed) ==
            gamenet::net::PostResult::Accepted);
        GAMENET_TEST_ASSERT(
            reconnectResult.load(std::memory_order_relaxed) ==
            gamenet::net::PostResult::QueueFull);
        loop.runAfter(50ms, [&] { server.start(); });
    };
    loop.runAfter(150ms, issueRequests);
    loop.runAfter(750ms, [&] {
        GAMENET_TEST_ASSERT(serverConnected == 0);
        server.stop();
        loop.quit();
    });
    loop.loop();

    GAMENET_TEST_ASSERT(
        disconnectResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        reconnectResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::QueueFull);
    GAMENET_TEST_ASSERT(serverConnected == 0);
}

void testAcceptedStopThenConnectSupersedesInflightAttempt() {
    gamenet::net::EventLoopOptions loopOptions;
    loopOptions.maxPendingFunctors = 4;
    loopOptions.reservedPendingFunctors = 0;
    loopOptions.maxFunctorsPerIteration = 4;
    gamenet::net::EventLoop loop(loopOptions);
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-supersede-server");
    gamenet::net::ConnectorOptions connectorOptions;
    connectorOptions.connectTimeout = 500ms;
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-supersede-client",
        connectorOptions);

    std::atomic<gamenet::net::PostResult> stopResult{
        gamenet::net::PostResult::OwnerUnavailable};
    std::atomic<gamenet::net::PostResult> connectResult{
        gamenet::net::PostResult::OwnerUnavailable};
    int clientConnected = 0;
    int clientDisconnected = 0;
    int serverConnected = 0;
    int serverDisconnected = 0;

    auto maybeFinish = [&] {
        if (clientDisconnected == 1 && serverDisconnected == 1) {
            server.stop();
            loop.quit();
        }
    };
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++serverConnected;
            } else {
                ++serverDisconnected;
                maybeFinish();
            }
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++clientConnected;
                connection->forceClose();
                return;
            }
            ++clientDisconnected;
            maybeFinish();
        });

    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);
    loop.queueInLoop([&] {
        // This functor is drained in the same batch immediately after the
        // first connect request starts its Connector attempt, before the next
        // poll round can process that attempt's completion.
        gamenet::test::runFromNonOwnerThread([&] {
            stopResult.store(client.tryStop(), std::memory_order_relaxed);
            connectResult.store(client.tryConnect(), std::memory_order_relaxed);
        });
        // Make the latest accepted connect immediately serviceable before the
        // next poll round can publish the superseded attempt.
        server.start();
    });

    loop.runAfter(3s, [&] {
        GAMENET_TEST_FAIL(
            "accepted stop-connect supersession did not establish latest request");
    });
    loop.loop();

    GAMENET_TEST_ASSERT(
        stopResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(
        connectResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(clientConnected == 1);
    GAMENET_TEST_ASSERT(clientDisconnected == 1);
    GAMENET_TEST_ASSERT(serverDisconnected == 1);
}

void testTerminalFailureAndCrossThreadFacade() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-terminal-server");
    gamenet::net::ConnectorOptions options;
    options.initRetryDelay = 75ms;
    options.maxRetryDelay = 300ms;
    options.connectTimeout = 250ms;
    options.enableRetry = false;
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-terminal-client",
        options);

    std::atomic<gamenet::net::PostResult> firstResult{
        gamenet::net::PostResult::OwnerUnavailable};
    int terminalFailures = 0;
    int clientConnected = 0;
    int clientDisconnected = 0;
    int serverDisconnected = 0;

    auto maybeFinish = [&] {
        if (clientDisconnected == 1 && serverDisconnected == 1) {
            server.stop();
            loop.quit();
        }
    };
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (!connection->connected()) {
                ++serverDisconnected;
                maybeFinish();
            }
        });
    client.setTerminalConnectFailureCallback(
        [&](const gamenet::net::InetAddress& address,
            gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            GAMENET_TEST_ASSERT(address.port() == server.listenAddress().port());
            GAMENET_TEST_ASSERT(
                event == gamenet::net::ConnectorEvent::ConnectFailed ||
                event == gamenet::net::ConnectorEvent::ConnectTimeout);
            ++terminalFailures;
            server.start();
            GAMENET_TEST_ASSERT(
                client.tryConnect() == gamenet::net::PostResult::Accepted);
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++clientConnected;
                connection->forceClose();
                return;
            }
            ++clientDisconnected;
            maybeFinish();
        });

    gamenet::test::runFromNonOwnerThread([&] {
        firstResult.store(client.tryConnect(), std::memory_order_relaxed);
    });
    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "timed out waiting for terminal failure re-entrant connect");

    GAMENET_TEST_ASSERT(
        firstResult.load(std::memory_order_relaxed) ==
        gamenet::net::PostResult::Accepted);
    GAMENET_TEST_ASSERT(terminalFailures == 1);
    GAMENET_TEST_ASSERT(clientConnected == 1);
    GAMENET_TEST_ASSERT(clientDisconnected == 1);
    GAMENET_TEST_ASSERT(serverDisconnected == 1);
}

void testQueuedFacadeTargetExpiry() {
    gamenet::net::EventLoop loop;
    std::atomic<int> callbacks{0};
    {
        auto client = std::make_unique<gamenet::net::TcpClient>(
            &loop,
            gamenet::net::InetAddress("127.0.0.1", 19998),
            "connector-expired-target-client");
        client->setTerminalConnectFailureCallback(
            [&](const gamenet::net::InetAddress&, gamenet::net::ConnectorEvent) {
                callbacks.fetch_add(1, std::memory_order_relaxed);
            });

        gamenet::net::PostResult result =
            gamenet::net::PostResult::OwnerUnavailable;
        gamenet::test::runFromNonOwnerThread([&] {
            result = client->tryConnect();
        });
        GAMENET_TEST_ASSERT(result == gamenet::net::PostResult::Accepted);
        client.reset();
    }

    loop.runAfter(20ms, [&] { loop.quit(); });
    loop.loop();
    GAMENET_TEST_ASSERT(callbacks.load(std::memory_order_relaxed) == 0);
}

void testExplicitConnectSurvivesDisconnectingConnectionRemoval() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-disconnect-reconnect-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-disconnect-reconnect-client");

    int clientConnected = 0;
    int clientDisconnected = 0;
    int serverConnected = 0;
    int serverDisconnected = 0;
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++serverConnected;
            } else {
                ++serverDisconnected;
            }
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++clientConnected;
                if (clientConnected == 1) {
                    GAMENET_TEST_ASSERT(
                        client.tryDisconnect() ==
                        gamenet::net::PostResult::Accepted);
                    GAMENET_TEST_ASSERT(
                        client.tryConnect() ==
                        gamenet::net::PostResult::Accepted);
                } else {
                    GAMENET_TEST_ASSERT(
                        client.tryDisconnect() ==
                        gamenet::net::PostResult::Accepted);
                }
                return;
            }

            ++clientDisconnected;
            if (clientConnected == 2 && clientDisconnected == 2) {
                loop.quit();
            }
        });

    server.start();
    GAMENET_TEST_ASSERT(!client.retry());
    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);
    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "explicit connect was lost while the old connection disconnected");

    GAMENET_TEST_ASSERT(clientConnected == 2);
    GAMENET_TEST_ASSERT(clientDisconnected == 2);
    GAMENET_TEST_ASSERT(serverConnected == 2);
    GAMENET_TEST_ASSERT(serverDisconnected == 2);
}

void testExplicitConnectFromDisconnectedCallbackStartsFreshLifecycle() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "connector-disconnected-reconnect-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        "connector-disconnected-reconnect-client");

    int clientConnected = 0;
    int clientDisconnected = 0;
    int serverConnected = 0;
    int serverDisconnected = 0;
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++serverConnected;
                if (serverConnected == 1) {
                    connection->forceClose();
                }
                return;
            }
            ++serverDisconnected;
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++clientConnected;
                if (clientConnected == 2) {
                    GAMENET_TEST_ASSERT(
                        client.tryDisconnect() ==
                        gamenet::net::PostResult::Accepted);
                }
                return;
            }

            ++clientDisconnected;
            if (clientDisconnected == 1) {
                GAMENET_TEST_ASSERT(
                    client.tryConnect() ==
                    gamenet::net::PostResult::Accepted);
                return;
            }
            server.stop();
            loop.quit();
        });

    server.start();
    GAMENET_TEST_ASSERT(!client.retry());
    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);
    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "explicit connect from disconnected callback was coalesced away");

    GAMENET_TEST_ASSERT(clientConnected == 2);
    GAMENET_TEST_ASSERT(clientDisconnected == 2);
    GAMENET_TEST_ASSERT(serverConnected == 2);
    GAMENET_TEST_ASSERT(serverDisconnected == 2);
}

#ifdef _WIN32
void runIocpHandoffRollbackCase(bool failPreserve) {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        failPreserve
            ? "connector-preserve-rollback-server"
            : "connector-registration-rollback-server");
    gamenet::net::TcpClient client(
        &loop,
        server.listenAddress(),
        failPreserve
            ? "connector-preserve-rollback-client"
            : "connector-registration-rollback-client");

    int terminalFailures = 0;
    int clientConnected = 0;
    int clientDisconnected = 0;
    client.setTerminalConnectFailureCallback(
        [&](const gamenet::net::InetAddress&,
            gamenet::net::ConnectorEvent event) {
            GAMENET_TEST_ASSERT(
                event == gamenet::net::ConnectorEvent::ConnectFailed);
            ++terminalFailures;
            GAMENET_TEST_ASSERT(!client.connection());
            const auto faultSocket =
                gamenet::net::detail::
                    EventLoopIocpAssociationHarness::lastFaultSocket();
            GAMENET_TEST_ASSERT(
                !gamenet::net::detail::
                    EventLoopIocpAssociationHarness::tracks(
                        loop,
                        faultSocket));
            GAMENET_TEST_ASSERT(
                client.tryConnect() ==
                gamenet::net::PostResult::Accepted);
        });
    client.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                ++clientConnected;
                GAMENET_TEST_ASSERT(
                    client.tryDisconnect() ==
                    gamenet::net::PostResult::Accepted);
                return;
            }
            ++clientDisconnected;
            server.stop();
            loop.quit();
        });

    server.start();
    if (failPreserve) {
        gamenet::net::detail::
            EventLoopIocpAssociationHarness::failNextPreserve();
    } else {
        gamenet::net::detail::
            EventLoopIocpAssociationHarness::
                failNextReplacementRegistration();
    }
    GAMENET_TEST_ASSERT(
        client.tryConnect() == gamenet::net::PostResult::Accepted);
    gamenet::test::runLoopWithTimeout(
        loop,
        3s,
        "IOCP connected-fd handoff rollback did not converge");

    GAMENET_TEST_ASSERT(terminalFailures == 1);
    GAMENET_TEST_ASSERT(clientConnected == 1);
    GAMENET_TEST_ASSERT(clientDisconnected == 1);
}

void testIocpHandoffFailuresRollBackTransactionally() {
    runIocpHandoffRollbackCase(true);
    runIocpHandoffRollbackCase(false);
}
#endif

}  // namespace

int main() {
    testTypedExecutorAdmission();
    testConnectorOwnerOnlyBoundary();
    testConnectSuccessObserverMayStopReentrantly();
    testConnectSuccessObserverMayRestartReentrantly();
    testConnectSuccessObserverMayStopThenStartReentrantly();
    testThrowingNewConnectionCallbackClosesUnpublishedFd();
    testRestartRestoresConfiguredInitialDelayAndRejectsStaleRetry();
    testRejectedStopDoesNotSupersedeAcceptedConnect();
    testRejectedConnectDoesNotSupersedeAcceptedDisconnect();
    testAcceptedStopThenConnectSupersedesInflightAttempt();
    testTerminalFailureAndCrossThreadFacade();
    testQueuedFacadeTargetExpiry();
    testExplicitConnectSurvivesDisconnectingConnectionRemoval();
    testExplicitConnectFromDisconnectedCallbackStartsFreshLifecycle();
#ifdef _WIN32
    testIocpHandoffFailuresRollBackTransactionally();
#endif
    return 0;
}
