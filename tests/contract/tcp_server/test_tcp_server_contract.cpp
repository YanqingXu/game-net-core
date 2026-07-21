#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/CallbackException.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool callbackTestWriteEventually(gamenet::net::SocketFd fd, std::string_view payload) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(fd, payload.data(), payload.size());
        if (written == static_cast<ssize_t>(payload.size())) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool callbackTestWaitForClose(gamenet::net::SocketFd fd) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    char buffer[64]{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto received = gamenet::net::sockets::read(fd, buffer, sizeof(buffer));
        if (received == 0) {
            return true;
        }
        if (received < 0) {
            const int error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInterrupted(error)) {
                return true;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool callbackTestReadEventually(gamenet::net::SocketFd fd, std::string_view expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::string received;
    char buffer[64]{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            received.append(buffer, static_cast<std::size_t>(count));
            if (received.find(expected) != std::string::npos) {
                return true;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

template <typename Predicate>
bool callbackTestWaitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

constexpr unsigned callbackSourceBit(gamenet::net::TcpConnectionCallbackSource source) {
    return 1u << static_cast<unsigned>(source);
}

constexpr unsigned admissionEventBit(gamenet::net::TcpServerAdmissionEvent event) {
    return 1u << static_cast<unsigned>(event);
}

}  // namespace

int main() {
    {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "server-contract");
    server.setConnectionBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
        .lowWaterMarkBytes = 128,
        .highWaterMarkBytes = 256,
        .hardLimitBytes = 512,
    });

    bool connected = false;
    bool disconnected = false;
    bool overloadObserved = false;

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        GAMENET_TEST_ASSERT(loop.isInLoopThread());

        if (conn->connected()) {
            connected = true;
            GAMENET_TEST_ASSERT(server.connectionCount() == 1);
            overloadObserved =
                conn->trySend(std::string(513, 'o')) == gamenet::net::TcpSendResult::Overloaded;
            conn->forceClose();
            return;
        }

        disconnected = true;
        loop.queueInLoop([&] {
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            server.stop();
            loop.quit();
        });
    });

    server.start();
    gamenet::net::SocketFd clientFd = gamenet::test::connectTestClient(server.listenAddress());

    gamenet::test::runLoopWithTimeout(
        loop,
        std::chrono::seconds(1),
        "timed out waiting for tcp server lifecycle");

    gamenet::test::closeTestSocket(clientFd);

    GAMENET_TEST_ASSERT(connected);
    GAMENET_TEST_ASSERT(disconnected);
    GAMENET_TEST_ASSERT(overloadObserved);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    }

    {
        gamenet::net::EventLoop callbackLoop;
        gamenet::net::TcpServer callbackServer(
            &callbackLoop,
            gamenet::net::InetAddress(0, true),
            "callback-exception-server");
        callbackServer.setThreadNum(1);

        std::atomic<int> messageCount{0};
        std::atomic<int> disconnectedCount{0};
        std::atomic<unsigned> observedSources{0};
        std::atomic<bool> observerRanOnOwner{true};

        callbackServer.setCallbackExceptionHandler(
            [&](const gamenet::net::TcpConnection& connection,
                const gamenet::net::TcpConnectionCallbackException& failure) {
                observerRanOnOwner.store(
                    observerRanOnOwner.load() && connection.getLoop()->isInLoopThread());
                observedSources.fetch_or(callbackSourceBit(failure.source));
                GAMENET_TEST_ASSERT(failure.exception != nullptr);
                if (failure.source == gamenet::net::TcpConnectionCallbackSource::Message) {
                    throw std::runtime_error("observer failure must be contained");
                }
            });
        callbackServer.setConnectionCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection) {
                if (!connection->connected() && disconnectedCount.fetch_add(1) == 0) {
                    throw std::runtime_error("disconnect callback failure");
                }
            });
        callbackServer.setMessageCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection,
                gamenet::net::Buffer* input) {
                input->retrieveAll();
                if (messageCount.fetch_add(1) == 0) {
                    throw std::runtime_error("message callback failure");
                }
                connection->send("served");
            });

        callbackServer.start();
        std::atomic<bool> clientSucceeded{false};
        std::thread callbackClient([&] {
            auto first = gamenet::test::connectTestClient(callbackServer.listenAddress());
            const bool firstClosed = callbackTestWriteEventually(first, "boom") &&
                callbackTestWaitForClose(first);
            gamenet::test::closeTestSocket(first);

            auto second = gamenet::test::connectTestClient(callbackServer.listenAddress());
            const bool secondServed = callbackTestWriteEventually(second, "ok") &&
                callbackTestReadEventually(second, "served");
            gamenet::test::closeTestSocket(second);

            clientSucceeded.store(firstClosed && secondServed);
            callbackServer.stop();
            callbackLoop.quit();
        });

        gamenet::test::runLoopWithTimeout(
            callbackLoop,
            5s,
            "timed out waiting for callback exception isolation");
        callbackClient.join();

        const unsigned sources = observedSources.load();
        GAMENET_TEST_ASSERT(clientSucceeded.load());
        GAMENET_TEST_ASSERT(messageCount.load() >= 2);
        GAMENET_TEST_ASSERT(disconnectedCount.load() >= 1);
        GAMENET_TEST_ASSERT(
            (sources & callbackSourceBit(
                gamenet::net::TcpConnectionCallbackSource::Message)) != 0);
        GAMENET_TEST_ASSERT(
            (sources & callbackSourceBit(
                gamenet::net::TcpConnectionCallbackSource::Disconnected)) != 0);
        GAMENET_TEST_ASSERT(observerRanOnOwner.load());
        GAMENET_TEST_ASSERT(callbackServer.connectionCount() == 0);
    }

    {
        gamenet::net::TcpServerAdmissionOptions invalidOptions;
        invalidOptions.maxConnectionAttemptsPerPeerPerWindow = 1;
        invalidOptions.maxTrackedPeerAddresses = 0;
        bool rejectedInvalidOptions = false;
        try {
            invalidOptions.validate();
        } catch (const std::invalid_argument&) {
            rejectedInvalidOptions = true;
        }
        GAMENET_TEST_ASSERT(rejectedInvalidOptions);

        gamenet::net::EventLoop limitLoop;
        gamenet::net::TcpServer limitServer(
            &limitLoop,
            gamenet::net::InetAddress(0, true),
            "global-admission-server");
        limitServer.setAdmissionOptions(gamenet::net::TcpServerAdmissionOptions{
            .maxConnections = 1,
        });

        std::atomic<unsigned> observedAdmissionEvents{0};
        limitServer.setAdmissionMetricCallback(
            [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
                GAMENET_TEST_ASSERT(limitLoop.isInLoopThread());
                observedAdmissionEvents.fetch_or(admissionEventBit(metric.event));
            });
        limitServer.start();

        std::atomic<bool> limitClientSucceeded{false};
        std::thread limitClient([&] {
            auto first = gamenet::test::connectTestClient(limitServer.listenAddress());
            const bool firstAccepted = callbackTestWaitUntil([&] {
                return limitServer.admissionStats().accepted == 1;
            });

            auto second = gamenet::test::connectTestClient(limitServer.listenAddress());
            const bool secondRejected = callbackTestWaitUntil([&] {
                return limitServer.admissionStats().rejectedConnectionLimit == 1;
            });
            const bool secondClosed = secondRejected && callbackTestWaitForClose(second);
            gamenet::test::closeTestSocket(second);

            gamenet::test::closeTestSocket(first);
            const bool accountingReleased = callbackTestWaitUntil([&] {
                return limitServer.admissionStats().activeConnections == 0;
            });
            limitClientSucceeded.store(
                firstAccepted && secondRejected && secondClosed && accountingReleased);
            limitServer.stop();
            limitLoop.quit();
        });

        gamenet::test::runLoopWithTimeout(
            limitLoop,
            5s,
            "timed out waiting for global connection admission");
        limitClient.join();

        const auto stats = limitServer.admissionStats();
        GAMENET_TEST_ASSERT(limitClientSucceeded.load());
        GAMENET_TEST_ASSERT(stats.accepted == 1);
        GAMENET_TEST_ASSERT(stats.rejectedConnectionLimit == 1);
        GAMENET_TEST_ASSERT(stats.activeConnections == 0);
        GAMENET_TEST_ASSERT(
            (observedAdmissionEvents.load() & admissionEventBit(
                gamenet::net::TcpServerAdmissionEvent::RejectedConnectionLimit)) != 0);
    }

    {
        gamenet::net::EventLoop admissionLoop;
        gamenet::net::TcpServer admissionServer(
            &admissionLoop,
            gamenet::net::InetAddress(0, true),
            "peer-auth-admission-server");
        admissionServer.setThreadNum(1);
        admissionServer.setAdmissionOptions(gamenet::net::TcpServerAdmissionOptions{
            .maxConnectionsPerPeer = 1,
            .maxConnectionAttemptsPerPeerPerWindow = 3,
            .connectionAttemptWindow = 2s,
            .maxTrackedPeerAddresses = 1,
            .unauthenticatedTimeout = 120ms,
        });

        std::atomic<unsigned> observedAdmissionEvents{0};
        admissionServer.setAdmissionMetricCallback(
            [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
                GAMENET_TEST_ASSERT(admissionLoop.isInLoopThread());
                observedAdmissionEvents.fetch_or(admissionEventBit(metric.event));
                if (metric.event ==
                    gamenet::net::TcpServerAdmissionEvent::RejectedPerPeerRateLimit) {
                    throw std::runtime_error(
                        "admission metric failure must be contained");
                }
            });
        admissionServer.setMessageCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection,
                gamenet::net::Buffer* input) {
                const std::string payload = input->retrieveAllAsString();
                if (payload == "AUTH") {
                    GAMENET_TEST_ASSERT(
                        admissionServer.tryMarkConnectionAuthenticated(connection));
                    connection->send("AUTH_OK");
                    return;
                }
                connection->send("ALIVE");
            });
        admissionServer.start();

        std::atomic<bool> admissionClientSucceeded{false};
        std::thread admissionClient([&] {
            auto authenticated =
                gamenet::test::connectTestClient(admissionServer.listenAddress());
            const bool authSucceeded = callbackTestWriteEventually(authenticated, "AUTH") &&
                callbackTestReadEventually(authenticated, "AUTH_OK") &&
                callbackTestWaitUntil([&] {
                    return admissionServer.admissionStats().authenticated == 1;
                });

            auto overPeerLimit =
                gamenet::test::connectTestClient(admissionServer.listenAddress());
            const bool peerRejected = callbackTestWaitUntil([&] {
                return admissionServer.admissionStats().rejectedPerPeerLimit == 1;
            });
            const bool peerClosed = peerRejected && callbackTestWaitForClose(overPeerLimit);
            gamenet::test::closeTestSocket(overPeerLimit);

            std::this_thread::sleep_for(180ms);
            const bool authenticatedStayedOpen =
                callbackTestWriteEventually(authenticated, "PING") &&
                callbackTestReadEventually(authenticated, "ALIVE");
            gamenet::test::closeTestSocket(authenticated);
            const bool authenticatedReleased = callbackTestWaitUntil([&] {
                return admissionServer.admissionStats().activeConnections == 0;
            });

            auto unauthenticated =
                gamenet::test::connectTestClient(admissionServer.listenAddress());
            const bool timeoutObserved = callbackTestWaitUntil([&] {
                return admissionServer.admissionStats().authenticationTimedOut == 1;
            });
            const bool unauthenticatedClosed =
                timeoutObserved && callbackTestWaitForClose(unauthenticated);
            gamenet::test::closeTestSocket(unauthenticated);
            const bool timeoutReleased = callbackTestWaitUntil([&] {
                return admissionServer.admissionStats().activeConnections == 0;
            });

            auto rateLimited =
                gamenet::test::connectTestClient(admissionServer.listenAddress());
            const bool rateRejected = callbackTestWaitUntil([&] {
                return admissionServer.admissionStats().rejectedPerPeerRateLimit == 1;
            });
            const bool rateClosed = rateRejected && callbackTestWaitForClose(rateLimited);
            gamenet::test::closeTestSocket(rateLimited);

            admissionClientSucceeded.store(
                authSucceeded && peerRejected && peerClosed &&
                authenticatedStayedOpen && authenticatedReleased &&
                timeoutObserved && unauthenticatedClosed && timeoutReleased &&
                rateRejected && rateClosed);
            admissionServer.stop();
            admissionLoop.quit();
        });

        gamenet::test::runLoopWithTimeout(
            admissionLoop,
            8s,
            "timed out waiting for peer/authentication admission controls");
        admissionClient.join();

        const auto stats = admissionServer.admissionStats();
        const unsigned events = observedAdmissionEvents.load();
        GAMENET_TEST_ASSERT(admissionClientSucceeded.load());
        GAMENET_TEST_ASSERT(stats.accepted == 2);
        GAMENET_TEST_ASSERT(stats.rejectedPerPeerLimit == 1);
        GAMENET_TEST_ASSERT(stats.rejectedPerPeerRateLimit == 1);
        GAMENET_TEST_ASSERT(stats.authenticated == 1);
        GAMENET_TEST_ASSERT(stats.authenticationTimedOut == 1);
        GAMENET_TEST_ASSERT(stats.activeConnections == 0);
        GAMENET_TEST_ASSERT(
            (events & admissionEventBit(
                gamenet::net::TcpServerAdmissionEvent::Authenticated)) != 0);
        GAMENET_TEST_ASSERT(
            (events & admissionEventBit(
                gamenet::net::TcpServerAdmissionEvent::AuthenticationTimedOut)) != 0);
        GAMENET_TEST_ASSERT(
            (events & admissionEventBit(
                gamenet::net::TcpServerAdmissionEvent::RejectedPerPeerLimit)) != 0);
        GAMENET_TEST_ASSERT(
            (events & admissionEventBit(
                gamenet::net::TcpServerAdmissionEvent::RejectedPerPeerRateLimit)) != 0);
    }

    return 0;
}
