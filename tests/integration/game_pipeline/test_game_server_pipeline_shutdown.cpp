#include "game_server_pipeline_demo/GameServerPipeline.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TestAssert.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace gamenet::examples {

class GameServerPipelineShutdownTestPeer {
public:
    static std::size_t pendingAuthenticationTimers(const GameServerPipeline& pipeline) {
        std::size_t count = 0;
        for (const auto& [name, state] : pipeline.connections_) {
            (void)name;
            if (state.authenticationTimer) ++count;
        }
        return count;
    }

    static std::weak_ptr<void> pendingAuthenticationAttempt(
        const GameServerPipeline& pipeline) {
        for (const auto& [name, state] : pipeline.connections_) {
            (void)name;
            if (state.authenticationAttempt) return state.authenticationAttempt;
        }
        return {};
    }

    static bool logicStopWaitActive(const GameServerPipeline& pipeline) {
        return pipeline.logicStopWaitActive_.load(std::memory_order_acquire);
    }
};

}  // namespace gamenet::examples

namespace {

void destroyWithWorkerCallbacksPending() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    std::promise<void> ioCallbackEntered;
    auto ioCallbackFuture = ioCallbackEntered.get_future();
    std::promise<void> releaseIoCallback;
    auto releaseFuture = releaseIoCallback.get_future().share();
    std::atomic<bool> blockedIoCallback{false};
    std::atomic<bool> ioCallbackContinued{false};
    auto pipeline = std::make_unique<gamenet::examples::GameServerPipeline>(
        &loop,
        gamenet::net::InetAddress(0, true),
        gamenet::examples::GameServerPipelineOptions{
            .ioThreads = 1,
            .stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
                if (stage != gamenet::examples::GameServerPipelineStage::Io ||
                    blockedIoCallback.exchange(true)) {
                    return;
                }
                ioCallbackEntered.set_value();
                releaseFuture.wait();
                ioCallbackContinued.store(true, std::memory_order_release);
            }});
    pipeline->start();

    gamenet::protocol::PacketFramer codec;
    std::thread destroyCoordinator;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline->listenAddress(), "pipeline-worker-destruction-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        const auto auth = codec.encode("AUTH destruction-player");
        const auto command = codec.encode("queued-during-destruction");
        GAMENET_TEST_ASSERT(auth && command);
        connection->send(*auth + *command);
        destroyCoordinator = std::thread([&] {
            if (ioCallbackFuture.wait_for(2s) != std::future_status::ready) {
                loop.queueInLoop(
                    [&] { GAMENET_TEST_FAIL("worker I/O callback did not enter before destruction"); });
                return;
            }
            loop.queueInLoop([&] {
                pipeline->stop();
                std::thread releaseAfterMemberTeardown([&] {
                    std::this_thread::sleep_for(10ms);
                    releaseIoCallback.set_value();
                });
                pipeline.reset();
                releaseAfterMemberTeardown.join();
                GAMENET_TEST_ASSERT(ioCallbackContinued.load(std::memory_order_acquire));
                client->stop();
                loop.runAfter(10ms, [&] { loop.quit(); });
            });
        });
    });
    client->connect();
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("worker callback destruction timed out"); });
    loop.loop();
    if (destroyCoordinator.joinable()) destroyCoordinator.join();
    GAMENET_TEST_ASSERT(!pipeline);
}

void stopSynchronouslyFromLogicStage() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline* pipelineObserver = nullptr;
    bool logicStageEntered = false;
    bool synchronousStopReturned = false;

    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
            if (stage != gamenet::examples::GameServerPipelineStage::Logic || logicStageEntered) {
                return;
            }
            logicStageEntered = true;
            pipelineObserver->stop();
            synchronousStopReturned = true;
            loop.runAfter(10ms, [&] { loop.quit(); });
        }});
    pipelineObserver = &pipeline;
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    gamenet::protocol::PacketFramer decoder;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), "pipeline-logic-stage-stop-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        const auto auth = codec.encode("AUTH logic-stop-player");
        GAMENET_TEST_ASSERT(auth);
        connection->send(*auth);
    });
    client->setMessageCallback([&](const auto& connection, auto* buffer) {
        auto parsed = decoder.push(buffer->retrieveAllAsString());
        for (const auto& response : parsed.frames) {
            if (response != "AUTH_OK") continue;
            const auto command = codec.encode("stop-inside-logic-observer");
            GAMENET_TEST_ASSERT(command);
            connection->send(*command);
        }
    });
    client->connect();
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("logic-stage synchronous stop timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT(logicStageEntered);
    GAMENET_TEST_ASSERT(synchronousStopReturned);
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}

void stopDistinctLogicLoopWhileCallbackActive() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop managementLoop;
    gamenet::net::EventLoopThread logicThread;
    auto* logicOwner = logicThread.startLoop();

    std::promise<void> logicCallbackEntered;
    auto logicCallbackEnteredFuture = logicCallbackEntered.get_future();
    std::promise<void> releaseLogicCallback;
    auto releaseLogicCallbackFuture = releaseLogicCallback.get_future().share();
    std::atomic<bool> logicCallbackBlocked{false};
    std::atomic<bool> logicCallbackExited{false};
    std::atomic<bool> stopReturned{false};
    std::atomic<bool> overlapObserved{false};
    std::atomic<bool> coordinatorTimedOut{false};

    gamenet::examples::GameServerPipeline pipeline(
        &managementLoop,
        gamenet::net::InetAddress(0, true),
        {.logicLoop = logicOwner,
         .stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
             if (stage != gamenet::examples::GameServerPipelineStage::Logic ||
                 logicCallbackBlocked.exchange(true, std::memory_order_acq_rel)) {
                 return;
             }
             logicCallbackEntered.set_value();
             releaseLogicCallbackFuture.wait();
             logicCallbackExited.store(true, std::memory_order_release);
         }});
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    gamenet::protocol::PacketFramer decoder;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &managementLoop, pipeline.listenAddress(), "pipeline-distinct-logic-stop-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        const auto auth = codec.encode("AUTH distinct-logic-stop-player");
        GAMENET_TEST_ASSERT(auth);
        connection->send(*auth);
    });
    client->setMessageCallback([&](const auto& connection, auto* buffer) {
        auto parsed = decoder.push(buffer->retrieveAllAsString());
        for (const auto& response : parsed.frames) {
            if (response != "AUTH_OK") continue;
            const auto command = codec.encode("hold-distinct-logic-callback");
            GAMENET_TEST_ASSERT(command);
            connection->send(*command);
        }
    });

    std::thread stopCoordinator([&] {
        if (logicCallbackEnteredFuture.wait_for(2s) != std::future_status::ready) {
            coordinatorTimedOut.store(true, std::memory_order_release);
            releaseLogicCallback.set_value();
            managementLoop.queueInLoop([&] {
                pipeline.stop();
                managementLoop.quit();
            });
            return;
        }

        managementLoop.queueInLoop([&] {
            pipeline.stop();
            stopReturned.store(true, std::memory_order_release);
            GAMENET_TEST_ASSERT(logicCallbackExited.load(std::memory_order_acquire));
            managementLoop.runAfter(10ms, [&] { managementLoop.quit(); });
        });

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!gamenet::examples::GameServerPipelineShutdownTestPeer::logicStopWaitActive(
                   pipeline) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (!gamenet::examples::GameServerPipelineShutdownTestPeer::logicStopWaitActive(
                pipeline)) {
            coordinatorTimedOut.store(true, std::memory_order_release);
        } else {
            overlapObserved.store(
                !logicCallbackExited.load(std::memory_order_acquire) &&
                    !stopReturned.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        releaseLogicCallback.set_value();
    });

    client->connect();
    managementLoop.runAfter(
        3s, [&] { GAMENET_TEST_FAIL("distinct logic callback stop timed out"); });
    managementLoop.loop();
    stopCoordinator.join();

    GAMENET_TEST_ASSERT(!coordinatorTimedOut.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(overlapObserved.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(logicCallbackBlocked.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(logicCallbackExited.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(stopReturned.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}

void cancelEndpointSendAcrossRevocation() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    std::promise<void> endpointObserverEntered;
    auto endpointObserverFuture = endpointObserverEntered.get_future();
    std::promise<void> releaseEndpointObserver;
    auto releaseEndpointFuture = releaseEndpointObserver.get_future().share();
    std::promise<void> stopCompleted;
    auto stopCompletedFuture = stopCompleted.get_future();
    std::atomic<bool> blockedEndpointObserver{false};
    std::atomic<bool> coordinatorTimedOut{false};
    std::size_t receivedBytes = 0;
    bool stopReturned = false;

    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.ioThreads = 1,
         .stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
             if (stage != gamenet::examples::GameServerPipelineStage::Endpoint ||
                 blockedEndpointObserver.exchange(true)) {
                 return;
             }
             endpointObserverEntered.set_value();
             releaseEndpointFuture.wait();
         }});
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), "pipeline-revoked-output-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        const auto auth = codec.encode("AUTH revoked-output-player");
        GAMENET_TEST_ASSERT(auth);
        connection->send(*auth);
    });
    client->setMessageCallback([&](const auto&, auto* buffer) {
        receivedBytes += buffer->readableBytes();
        buffer->retrieveAll();
    });

    std::thread stopCoordinator([&] {
        if (endpointObserverFuture.wait_for(2s) != std::future_status::ready) {
            coordinatorTimedOut.store(true, std::memory_order_release);
            releaseEndpointObserver.set_value();
            loop.queueInLoop([&] { loop.quit(); });
            return;
        }
        loop.queueInLoop([&] {
            pipeline.stop();
            stopReturned = true;
            stopCompleted.set_value();
        });
        if (stopCompletedFuture.wait_for(2s) != std::future_status::ready) {
            coordinatorTimedOut.store(true, std::memory_order_release);
        }
        releaseEndpointObserver.set_value();
        loop.queueInLoop([&] { loop.runAfter(50ms, [&] { loop.quit(); }); });
    });

    client->connect();
    loop.runAfter(3s, [&] { GAMENET_TEST_FAIL("revoked endpoint output timed out"); });
    loop.loop();
    stopCoordinator.join();

    GAMENET_TEST_ASSERT(!coordinatorTimedOut.load(std::memory_order_acquire));
    GAMENET_TEST_ASSERT(blockedEndpointObserver.load());
    GAMENET_TEST_ASSERT(stopReturned);
    GAMENET_TEST_ASSERT(receivedBytes == 0);
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    bool nullLoopRejected = false;
    try {
        gamenet::examples::GameServerPipeline invalid(
            nullptr, gamenet::net::InetAddress(0, true));
    } catch (const std::invalid_argument&) {
        nullLoopRejected = true;
    }
    GAMENET_TEST_ASSERT(nullLoopRejected);

    destroyWithWorkerCallbacksPending();
    stopSynchronouslyFromLogicStage();
    stopDistinctLogicLoopWhileCallbackActive();
    cancelEndpointSendAcrossRevocation();

    gamenet::net::EventLoop loop;
    gamenet::examples::GameServerPipeline* pipelineObserver = nullptr;
    bool stopQueued = false;
    bool stopCompleted = false;
    gamenet::examples::GameServerPipeline pipeline(
        &loop,
        gamenet::net::InetAddress(0, true),
        {.ioThreads = 1,
         .authenticationDelay = 100ms,
         .stageObserver = [&](gamenet::examples::GameServerPipelineStage stage) {
            if (stage != gamenet::examples::GameServerPipelineStage::Management || stopQueued) {
                 return;
             }
            stopQueued = true;
            loop.queueInLoop([&] {
                GAMENET_TEST_ASSERT(
                    gamenet::examples::GameServerPipelineShutdownTestPeer::pendingAuthenticationTimers(
                        *pipelineObserver) == 1);
                auto pendingAttempt =
                    gamenet::examples::GameServerPipelineShutdownTestPeer::pendingAuthenticationAttempt(
                        *pipelineObserver);
                GAMENET_TEST_ASSERT(!pendingAttempt.expired());
                pipelineObserver->stop();
                stopCompleted = true;
                GAMENET_TEST_ASSERT(
                    gamenet::examples::GameServerPipelineShutdownTestPeer::pendingAuthenticationTimers(
                        *pipelineObserver) == 0);
                GAMENET_TEST_ASSERT(pendingAttempt.expired());
                loop.runAfter(150ms, [&] { loop.quit(); });
            });
         }});
    pipelineObserver = &pipeline;
    pipeline.start();

    gamenet::protocol::PacketFramer codec;
    auto client = std::make_unique<gamenet::net::TcpClient>(
        &loop, pipeline.listenAddress(), "pipeline-shutdown-client");
    client->setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) return;
        const auto auth = codec.encode("AUTH shutdown-player");
        const auto command = codec.encode("queued-command");
        GAMENET_TEST_ASSERT(auth && command);
        connection->send(*auth + *command);
    });
    client->connect();
    loop.runAfter(2s, [&] { GAMENET_TEST_FAIL("pipeline shutdown timed out"); });
    loop.loop();

    GAMENET_TEST_ASSERT(stopQueued && stopCompleted);
    GAMENET_TEST_ASSERT(pipeline.activeSessionCount() == 0);
    bool restartRejected = false;
    try {
        pipeline.start();
    } catch (const std::logic_error&) {
        restartRejected = true;
    }
    GAMENET_TEST_ASSERT(restartRejected);
}
