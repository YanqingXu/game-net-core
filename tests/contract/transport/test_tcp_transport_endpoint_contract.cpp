#include "gamenet/transport/TcpTransportEndpoint.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "support/SocketPair.h"
#include "support/TcpConnectionHarness.h"
#include "support/TestAssert.h"

#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    gamenet::net::EventLoop loop;
    gamenet::test::ConnectedSocketPair pair;
    auto connection = gamenet::test::makeTcpConnection(loop, pair, "transport-endpoint");
    connection->setBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
        .lowWaterMarkBytes = 128,
        .highWaterMarkBytes = 256,
        .hardLimitBytes = 512,
    });
    auto endpoint = std::make_shared<gamenet::transport::TcpTransportEndpoint>(
        gamenet::transport::TransportSessionId{42}, connection);

    GAMENET_TEST_ASSERT(endpoint->id().value == 42);
    GAMENET_TEST_ASSERT(endpoint->ownerExecutor().id() == loop.executor().id());
    GAMENET_TEST_ASSERT(endpoint->ownerExecutor().available());
    GAMENET_TEST_ASSERT(!endpoint->isOpen());

    gamenet::transport::EndpointResult wrongThread{};
    std::thread caller([&] { wrongThread = endpoint->send("wrong-thread"); });
    caller.join();
    GAMENET_TEST_ASSERT(wrongThread == gamenet::transport::EndpointResult::WrongThread);

    gamenet::transport::EndpointResult wrongThreadClose{};
    std::thread closer([&] {
        wrongThreadClose = endpoint->close(gamenet::transport::CloseReason::Normal);
    });
    closer.join();
    GAMENET_TEST_ASSERT(wrongThreadClose == gamenet::transport::EndpointResult::WrongThread);

    const std::string payload = "transport-payload";
    std::size_t received = 0;
    bool closeRequested = false;
    connection->setCloseCallback([&](const gamenet::net::TcpConnectionPtr& conn) {
        conn->connectDestroyed();
        loop.quit();
    });
    connection->connectEstablished();
    GAMENET_TEST_ASSERT(endpoint->isOpen());
    GAMENET_TEST_ASSERT(
        endpoint->send(std::string(513, 'o')) ==
        gamenet::transport::EndpointResult::Overloaded);

    std::atomic<bool> observerSawClosed{false};
    std::atomic<bool> stopObserver{false};
    std::thread observer([&] {
        while (!stopObserver.load(std::memory_order_relaxed)) {
            if (!endpoint->isOpen()) {
                observerSawClosed.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });
    GAMENET_TEST_ASSERT(endpoint->send(payload) == gamenet::transport::EndpointResult::Accepted);

    loop.runEvery(1ms, [&] {
        char buffer[64];
        const auto count = gamenet::net::sockets::read(pair.peerFd, buffer, sizeof(buffer));
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        }
        if (received == payload.size() && !closeRequested) {
            closeRequested = true;
            GAMENET_TEST_ASSERT(
                endpoint->close(gamenet::transport::CloseReason::ProtocolError) ==
                gamenet::transport::EndpointResult::Accepted);
        }
    });
    loop.runAfter(2s, [&] {
        GAMENET_TEST_FAIL("transport endpoint contract timed out");
    });
    loop.loop();

    stopObserver.store(true, std::memory_order_relaxed);
    observer.join();

    GAMENET_TEST_ASSERT(received == payload.size());
    GAMENET_TEST_ASSERT(closeRequested);
    GAMENET_TEST_ASSERT(!endpoint->isOpen());
    GAMENET_TEST_ASSERT(observerSawClosed.load(std::memory_order_relaxed));
    connection.reset();
    GAMENET_TEST_ASSERT(
        endpoint->send("expired") == gamenet::transport::EndpointResult::Closed);
}
