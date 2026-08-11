#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include "../../../src/core/net/detail/EventLoopActiveBatchHarness.h"
#include "../../../src/core/net/detail/EventLoopLifecycleRegistry.h"
#include "../../../src/core/net/detail/TcpConnectionConstructionHarness.h"

#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

namespace {

bool peerClosed(gamenet::net::SocketFd fd) {
    char byte = 0;
    const auto count = gamenet::net::sockets::read(fd, &byte, 1);
    if (count == 0) {
        return true;
    }
    if (count > 0) {
        return false;
    }
    const int error = gamenet::net::sockets::lastError();
    return !gamenet::net::sockets::isWouldBlock(error) &&
        !gamenet::net::sockets::isInterrupted(error);
}

void verifyConstructionFailureKeepsBaseFdOwnership() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-construction-failure-contract");
    server.setThreadNum(1);

    std::atomic<int> connectionCallbacks{0};
    std::atomic<int> acceptedMetrics{0};
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr&) {
            connectionCallbacks.fetch_add(1, std::memory_order_relaxed);
        });
    server.setAdmissionMetricCallback(
        [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
            if (metric.event ==
                gamenet::net::TcpServerAdmissionEvent::Accepted) {
                acceptedMetrics.fetch_add(1, std::memory_order_relaxed);
            }
        });

    server.start();
    gamenet::net::detail::TcpConnectionConstructionHarness::
        failNextBeforeSocketClaim();
    const auto client =
        gamenet::test::connectTestClient(server.listenAddress());

    bool failedPeerClosed = false;
    bool stopStarted = false;
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult stopResult;
    const auto progress = loop.runEvery(1ms, [&] {
        if (!failedPeerClosed) {
            failedPeerClosed = peerClosed(client);
            if (!failedPeerClosed) {
                return;
            }

            GAMENET_TEST_ASSERT(
                gamenet::net::detail::TcpConnectionConstructionHarness::
                    failureWasConsumed());
            GAMENET_TEST_ASSERT(
                !gamenet::net::detail::TcpConnectionConstructionHarness::
                    failureObservedSocketOwner());
            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            const auto stats = server.admissionStats();
            GAMENET_TEST_ASSERT(stats.accepted == 0);
            GAMENET_TEST_ASSERT(stats.activeConnections == 0);
            GAMENET_TEST_ASSERT(
                connectionCallbacks.load(std::memory_order_relaxed) == 0);
            GAMENET_TEST_ASSERT(
                acceptedMetrics.load(std::memory_order_relaxed) == 0);
        }

        if (!stopStarted) {
            stopFuture = server.stopGracefully(
                gamenet::net::TcpServerStopOptions{
                    .drainTimeout = 500ms,
                });
            stopStarted = true;
            return;
        }
        if (stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        stopResult = stopFuture.get();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        5s,
        "construction failure did not preserve base fd ownership");
    loop.cancel(progress);
    gamenet::test::closeTestSocket(client);

    GAMENET_TEST_ASSERT(failedPeerClosed);
    GAMENET_TEST_ASSERT(
        stopResult.outcome == gamenet::net::TcpServerStopOutcome::Drained);
}

void verifyOwnerEstablishmentFailureRollsBackAndRecovers() {
    gamenet::net::EventLoop loop(gamenet::net::EventLoopOptions{
        .maxLifecycleNodes = 3,
    });
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-owner-establishment-failure-contract");

    int connectedCallbacks = 0;
    int disconnectedCallbacks = 0;
    int acceptedMetrics = 0;
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (connection->connected()) {
                ++connectedCallbacks;
                GAMENET_TEST_ASSERT(
                    connection->tryForceClose() ==
                    gamenet::net::PostResult::Accepted);
            } else {
                ++disconnectedCallbacks;
            }
        });
    server.setAdmissionMetricCallback(
        [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (metric.event ==
                gamenet::net::TcpServerAdmissionEvent::Accepted) {
                ++acceptedMetrics;
            }
        });

    server.start();
    auto capacityOccupant =
        gamenet::net::detail::EventLoopLifecycleRegistry::attach(
            loop,
            [] {});
    GAMENET_TEST_ASSERT(loop.attachedLifecycleNodeCount() == 3);

    const auto failedClient =
        gamenet::test::connectTestClient(server.listenAddress());
    gamenet::net::SocketFd healthyClient = gamenet::net::kInvalidSocket;
    bool failedPeerClosed = false;
    bool failureRolledBack = false;
    bool stopStarted = false;
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult stopResult;

    const auto progress = loop.runEvery(1ms, [&] {
        if (!failedPeerClosed) {
            failedPeerClosed = peerClosed(failedClient);
            if (!failedPeerClosed) {
                return;
            }
        }

        if (!failureRolledBack) {
            if (server.connectionCount() != 0 ||
                server.admissionStats().activeConnections != 0) {
                return;
            }
            GAMENET_TEST_ASSERT(server.admissionStats().accepted == 1);
            GAMENET_TEST_ASSERT(acceptedMetrics == 1);
            GAMENET_TEST_ASSERT(connectedCallbacks == 0);
            GAMENET_TEST_ASSERT(disconnectedCallbacks == 0);
            GAMENET_TEST_ASSERT(loop.callbackExceptionCount() == 0);

            gamenet::net::detail::EventLoopLifecycleRegistry::detach(
                loop,
                capacityOccupant);
            healthyClient =
                gamenet::test::connectTestClient(server.listenAddress());
            failureRolledBack = true;
            return;
        }

        if (!stopStarted) {
            if (connectedCallbacks != 1 ||
                disconnectedCallbacks != 1 ||
                server.connectionCount() != 0 ||
                server.admissionStats().activeConnections != 0) {
                return;
            }
            GAMENET_TEST_ASSERT(server.admissionStats().accepted == 2);
            GAMENET_TEST_ASSERT(acceptedMetrics == 2);
            stopFuture = server.stopGracefully(
                gamenet::net::TcpServerStopOptions{
                    .drainTimeout = 500ms,
                });
            stopStarted = true;
            return;
        }

        if (stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        stopResult = stopFuture.get();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        5s,
        "owner establishment failure did not roll back and recover");
    loop.cancel(progress);
    gamenet::test::closeTestSocket(failedClient);
    if (healthyClient != gamenet::net::kInvalidSocket) {
        gamenet::test::closeTestSocket(healthyClient);
    }

    GAMENET_TEST_ASSERT(failedPeerClosed);
    GAMENET_TEST_ASSERT(failureRolledBack);
    GAMENET_TEST_ASSERT(connectedCallbacks == 1);
    GAMENET_TEST_ASSERT(disconnectedCallbacks == 1);
    GAMENET_TEST_ASSERT(
        stopResult.outcome == gamenet::net::TcpServerStopOutcome::Drained);
}

}  // namespace

int main() {
    verifyConstructionFailureKeepsBaseFdOwnership();
    verifyOwnerEstablishmentFailureRollsBackAndRecovers();

    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "server-establishment-saturation-contract");
    server.setThreadNum(2);
    server.setLoopSelectionPolicy(
        gamenet::net::EventLoopSelectionPolicy::LeastConnections);
    server.setAdmissionOptions(gamenet::net::TcpServerAdmissionOptions{
        .maxConnectionsPerPeer = 1,
        .unauthenticatedTimeout = 100ms,
        .authenticationDeadlineResolution = 5ms,
    });

    std::mutex workerLoopsMutex;
    std::vector<gamenet::net::EventLoop*> workerLoops;
    server.setThreadInitCallback(
        [&](gamenet::net::EventLoop* selectedLoop) {
            gamenet::net::detail::EventLoopActiveBatchHarness::
                configurePendingFunctorCapacity(
                    *selectedLoop,
                    8,
                    4,
                    8);
            std::lock_guard lock(workerLoopsMutex);
            workerLoops.push_back(selectedLoop);
        });

    std::atomic<int> connectedCallbacks{0};
    std::atomic<int> disconnectedCallbacks{0};
    std::atomic<int> acceptedAdmissionMetrics{0};
    std::array<std::atomic<gamenet::net::EventLoop*>, 2>
        healthyConnectionOwners{};
    server.setAdmissionMetricCallback(
        [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
            GAMENET_TEST_ASSERT(loop.isInLoopThread());
            if (metric.event ==
                gamenet::net::TcpServerAdmissionEvent::Accepted) {
                acceptedAdmissionMetrics.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        });
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            GAMENET_TEST_ASSERT(connection->getLoop()->isInLoopThread());
            if (connection->connected()) {
                const int callbackIndex = connectedCallbacks.fetch_add(
                    1,
                    std::memory_order_relaxed);
                GAMENET_TEST_ASSERT(callbackIndex >= 0);
                GAMENET_TEST_ASSERT(callbackIndex < 2);
                healthyConnectionOwners[static_cast<std::size_t>(callbackIndex)]
                    .store(
                        connection->getLoop(),
                        std::memory_order_relaxed);
                GAMENET_TEST_ASSERT(
                    connection->tryForceClose() ==
                    gamenet::net::PostResult::Accepted);
            } else {
                disconnectedCallbacks.fetch_add(1, std::memory_order_relaxed);
            }
        });

    server.start();
    {
        std::lock_guard lock(workerLoopsMutex);
        GAMENET_TEST_ASSERT(workerLoops.size() == 2);
    }
    gamenet::net::EventLoop* saturatedWorker = workerLoops[0];
    gamenet::net::EventLoop* alternateWorker = workerLoops[1];
    GAMENET_TEST_ASSERT(saturatedWorker != nullptr);
    GAMENET_TEST_ASSERT(alternateWorker != nullptr);
    GAMENET_TEST_ASSERT(saturatedWorker != alternateWorker);
    GAMENET_TEST_ASSERT(saturatedWorker != &loop);
    GAMENET_TEST_ASSERT(alternateWorker != &loop);

    std::promise<void> blockerEnteredPromise;
    auto blockerEntered = blockerEnteredPromise.get_future();
    std::promise<void> releaseBlockerPromise;
    auto releaseBlocker = releaseBlockerPromise.get_future().share();
    GAMENET_TEST_ASSERT(saturatedWorker->tryQueueInLoop(
        [&] {
            blockerEnteredPromise.set_value();
            releaseBlocker.wait();
        }));
    GAMENET_TEST_ASSERT(blockerEntered.wait_for(2s) == std::future_status::ready);

    std::size_t saturatedFunctorCount = 0;
    for (;;) {
        try {
            saturatedWorker->queueInLoop([] {});
            ++saturatedFunctorCount;
        } catch (const std::overflow_error&) {
            break;
        }
    }
    GAMENET_TEST_ASSERT(saturatedFunctorCount == 12);
    GAMENET_TEST_ASSERT(
        saturatedWorker->pendingFunctorCount() == saturatedFunctorCount);
    const auto rejectedBeforeEstablishment =
        saturatedWorker->rejectedFunctorCount();

    const auto rejectedClient =
        gamenet::test::connectTestClient(server.listenAddress());
    std::array<gamenet::net::SocketFd, 2> healthyClients{
        gamenet::net::kInvalidSocket,
        gamenet::net::kInvalidSocket,
    };
    bool blockerReleased = false;
    bool rejectedPeerClosed = false;
    std::size_t healthyConnectCount = 0;
    std::chrono::steady_clock::time_point deadlineObservationEnd{};
    bool stopStarted = false;
    gamenet::net::TcpServerStopFuture stopFuture;
    gamenet::net::TcpServerStopResult stopResult;

    const auto progress = loop.runEvery(1ms, [&] {
        if (!blockerReleased) {
            if (saturatedWorker->rejectedFunctorCount() ==
                rejectedBeforeEstablishment) {
                return;
            }

            GAMENET_TEST_ASSERT(server.connectionCount() == 0);
            const auto rejectedStats = server.admissionStats();
            GAMENET_TEST_ASSERT(rejectedStats.accepted == 0);
            GAMENET_TEST_ASSERT(rejectedStats.activeConnections == 0);
            GAMENET_TEST_ASSERT(
                rejectedStats.authenticationTimedOut == 0);
            GAMENET_TEST_ASSERT(
                acceptedAdmissionMetrics.load(std::memory_order_relaxed) == 0);
            GAMENET_TEST_ASSERT(
                connectedCallbacks.load(std::memory_order_relaxed) == 0);
            GAMENET_TEST_ASSERT(
                disconnectedCallbacks.load(std::memory_order_relaxed) == 0);
            releaseBlockerPromise.set_value();
            blockerReleased = true;
            return;
        }

        if (!rejectedPeerClosed) {
            rejectedPeerClosed = peerClosed(rejectedClient);
            if (!rejectedPeerClosed) {
                return;
            }
        }

        if (healthyConnectCount == 0) {
            if (saturatedWorker->pendingFunctorCount() != 0) {
                return;
            }
            healthyClients[0] =
                gamenet::test::connectTestClient(server.listenAddress());
            healthyConnectCount = 1;
            return;
        }

        if (healthyConnectCount == 1) {
            if (connectedCallbacks.load(std::memory_order_relaxed) != 1 ||
                disconnectedCallbacks.load(std::memory_order_relaxed) != 1 ||
                server.connectionCount() != 0 ||
                server.admissionStats().activeConnections != 0) {
                return;
            }
            GAMENET_TEST_ASSERT(
                healthyConnectionOwners[0].load(std::memory_order_relaxed) ==
                alternateWorker);
            healthyClients[1] =
                gamenet::test::connectTestClient(server.listenAddress());
            healthyConnectCount = 2;
            return;
        }

        if (deadlineObservationEnd ==
            std::chrono::steady_clock::time_point{}) {
            if (connectedCallbacks.load(std::memory_order_relaxed) != 2 ||
                disconnectedCallbacks.load(std::memory_order_relaxed) != 2 ||
                server.connectionCount() != 0 ||
                server.admissionStats().activeConnections != 0) {
                return;
            }
            // The first rejected provisional load must have returned to zero:
            // least-connections therefore rotates from worker 1 back to the
            // formerly saturated worker for this second healthy connection.
            GAMENET_TEST_ASSERT(
                healthyConnectionOwners[1].load(std::memory_order_relaxed) ==
                saturatedWorker);
            deadlineObservationEnd =
                std::chrono::steady_clock::now() + 200ms;
            return;
        }

        if (!stopStarted) {
            if (std::chrono::steady_clock::now() < deadlineObservationEnd) {
                return;
            }
            const auto recoveredStats = server.admissionStats();
            GAMENET_TEST_ASSERT(recoveredStats.accepted == 2);
            GAMENET_TEST_ASSERT(recoveredStats.activeConnections == 0);
            GAMENET_TEST_ASSERT(
                recoveredStats.authenticationTimedOut == 0);
            GAMENET_TEST_ASSERT(
                acceptedAdmissionMetrics.load(std::memory_order_relaxed) == 2);
            stopFuture = server.stopGracefully(
                gamenet::net::TcpServerStopOptions{
                    .drainTimeout = 500ms,
                });
            stopStarted = true;
            return;
        }

        if (stopFuture.wait_for(0ms) != std::future_status::ready) {
            return;
        }
        stopResult = stopFuture.get();
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        10s,
        "worker establishment saturation did not roll back and recover");
    loop.cancel(progress);

    gamenet::test::closeTestSocket(rejectedClient);
    for (const auto healthyClient : healthyClients) {
        if (healthyClient != gamenet::net::kInvalidSocket) {
            gamenet::test::closeTestSocket(healthyClient);
        }
    }

    GAMENET_TEST_ASSERT(blockerReleased);
    GAMENET_TEST_ASSERT(rejectedPeerClosed);
    GAMENET_TEST_ASSERT(healthyConnectCount == 2);
    GAMENET_TEST_ASSERT(
        connectedCallbacks.load(std::memory_order_relaxed) == 2);
    GAMENET_TEST_ASSERT(
        disconnectedCallbacks.load(std::memory_order_relaxed) == 2);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
    GAMENET_TEST_ASSERT(server.admissionStats().activeConnections == 0);
    GAMENET_TEST_ASSERT(
        stopResult.outcome == gamenet::net::TcpServerStopOutcome::Drained);
    return 0;
}
