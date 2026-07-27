#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include "support/ClientSocket.h"
#include "support/LoopTest.h"
#include "support/TestAssert.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kHardOutputLimit = 4 * 1024 * 1024;
constexpr std::size_t kPressurePayloadBytes = 3 * 1024 * 1024;

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

bool writeEventually(gamenet::net::SocketFd fd, std::string_view payload) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (offset < payload.size() && std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(
            fd,
            payload.data() + offset,
            payload.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0) {
            const int error = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isWouldBlock(error) &&
                !gamenet::net::sockets::isInterrupted(error)) {
                return false;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    return offset == payload.size();
}

bool readEventually(gamenet::net::SocketFd fd, std::string_view expected) {
    std::string received;
    char buffer[256]{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            received.append(buffer, static_cast<std::size_t>(count));
            if (received.find(expected) != std::string::npos) {
                return true;
            }
            continue;
        }
        if (count == 0) {
            return false;
        }
        const int error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool waitForClose(gamenet::net::SocketFd fd) {
    char buffer[256]{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(fd, buffer, sizeof(buffer));
        if (count == 0) {
            return true;
        }
        if (count < 0) {
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

void resetOnClose(gamenet::net::SocketFd fd) {
    linger value{};
    value.l_onoff = 1;
    value.l_linger = 0;
#ifdef _WIN32
    const int result = ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_LINGER,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
    GAMENET_TEST_ASSERT(result != SOCKET_ERROR);
#else
    const int result = ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &value, sizeof(value));
    GAMENET_TEST_ASSERT(result == 0);
#endif
}

std::chrono::seconds environmentSeconds(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return 0s;
    }
    unsigned long long value = 0;
    const std::string_view text(raw);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    GAMENET_TEST_ASSERT(error == std::errc{} && end == text.data() + text.size());
    GAMENET_TEST_ASSERT(value > 0);
    GAMENET_TEST_ASSERT(
        value <= static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    return std::chrono::seconds(static_cast<long long>(value));
}

std::chrono::milliseconds environmentMilliseconds(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return 0ms;
    }
    unsigned long long value = 0;
    const std::string_view text(raw);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    GAMENET_TEST_ASSERT(error == std::errc{} && end == text.data() + text.size());
    GAMENET_TEST_ASSERT(value <= 60'000);
    return std::chrono::milliseconds(value);
}

void runFaultCycle() {
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "production-fault-injection");
    server.setThreadNum(2);
    server.setConnectionBackpressureOptions(gamenet::net::TcpConnectionBackpressureOptions{
        .lowWaterMarkBytes = 32 * 1024,
        .highWaterMarkBytes = 64 * 1024,
        .hardLimitBytes = kHardOutputLimit,
    });

    std::atomic<int> connected{0};
    std::atomic<int> disconnected{0};
    std::atomic<int> callbackFailures{0};
    std::atomic<int> highWaterEvents{0};
    std::atomic<bool> overloadRejected{false};
    std::atomic<bool> callbacksStayedOnOwner{true};
    const std::string oversizedPayload(kHardOutputLimit + 1, 'o');
    const std::string pressurePayload(kPressurePayloadBytes, 'p');

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (!connection->getLoop()->isInLoopThread()) {
            callbacksStayedOnOwner.store(false);
        }
        if (connection->connected()) {
            connected.fetch_add(1);
        } else {
            disconnected.fetch_add(1);
        }
    });
    server.setCallbackExceptionHandler(
        [&](const gamenet::net::TcpConnection& connection,
            const gamenet::net::TcpConnectionCallbackException& failure) {
            if (!connection.getLoop()->isInLoopThread()) {
                callbacksStayedOnOwner.store(false);
            }
            GAMENET_TEST_ASSERT(failure.exception != nullptr);
            callbackFailures.fetch_add(1);
        });
    server.setHighWaterMarkCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection, std::size_t bytes) {
            if (!connection->getLoop()->isInLoopThread()) {
                callbacksStayedOnOwner.store(false);
            }
            GAMENET_TEST_ASSERT(bytes >= 64 * 1024);
            highWaterEvents.fetch_add(1);
        },
        64 * 1024);
    server.setMessageCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* input) {
            if (!connection->getLoop()->isInLoopThread()) {
                callbacksStayedOnOwner.store(false);
            }
            const std::string command = input->retrieveAllAsString();
            if (command == "throw") {
                throw std::runtime_error("injected message callback failure");
            }
            if (command == "healthy") {
                connection->send("healthy-ok");
                return;
            }
            if (command == "pressure") {
                overloadRejected.store(
                    connection->trySend(oversizedPayload) ==
                    gamenet::net::TcpSendResult::Overloaded);
                connection->send(pressurePayload);
            }
        });
    server.start();

    gamenet::net::TcpServerStopResult stopResult;
    std::jthread clients([&] {
        auto resetClient = gamenet::test::connectTestClient(server.listenAddress());
        GAMENET_TEST_ASSERT(waitUntil([&] { return connected.load() >= 1; }));
        resetOnClose(resetClient);
        gamenet::test::closeTestSocket(resetClient);
        GAMENET_TEST_ASSERT(waitUntil([&] { return disconnected.load() >= 1; }));

        auto throwingClient = gamenet::test::connectTestClient(server.listenAddress());
        GAMENET_TEST_ASSERT(waitUntil([&] { return connected.load() >= 2; }));
        GAMENET_TEST_ASSERT(writeEventually(throwingClient, "throw"));
        GAMENET_TEST_ASSERT(waitForClose(throwingClient));
        gamenet::test::closeTestSocket(throwingClient);
        GAMENET_TEST_ASSERT(waitUntil([&] {
            return callbackFailures.load() == 1 && disconnected.load() >= 2;
        }));

        auto healthyClient = gamenet::test::connectTestClient(server.listenAddress());
        GAMENET_TEST_ASSERT(waitUntil([&] { return connected.load() >= 3; }));
        GAMENET_TEST_ASSERT(writeEventually(healthyClient, "healthy"));
        GAMENET_TEST_ASSERT(readEventually(healthyClient, "healthy-ok"));
        gamenet::test::closeTestSocket(healthyClient);
        GAMENET_TEST_ASSERT(waitUntil([&] { return disconnected.load() >= 3; }));

        auto slowReader = gamenet::test::connectTestClientWithReceiveBuffer(
            server.listenAddress(),
            1024);
        GAMENET_TEST_ASSERT(waitUntil([&] { return connected.load() >= 4; }));
        GAMENET_TEST_ASSERT(writeEventually(slowReader, "pressure"));
        GAMENET_TEST_ASSERT(waitUntil([&] {
            return overloadRejected.load() && highWaterEvents.load() >= 1;
        }));

        const auto stopFuture = server.stopGracefully(gamenet::net::TcpServerStopOptions{
            .drainTimeout = 150ms,
        });
        GAMENET_TEST_ASSERT(stopFuture.wait_for(5s) == std::future_status::ready);
        stopResult = stopFuture.get();
        gamenet::test::closeTestSocket(slowReader);
        loop.quit();
    });

    gamenet::test::runLoopWithTimeout(
        loop,
        12s,
        "timed out running production fault-injection cycle");
    clients.join();

    GAMENET_TEST_ASSERT(connected.load() == 4);
    GAMENET_TEST_ASSERT(disconnected.load() == 4);
    GAMENET_TEST_ASSERT(callbackFailures.load() == 1);
    GAMENET_TEST_ASSERT(overloadRejected.load());
    GAMENET_TEST_ASSERT(highWaterEvents.load() >= 1);
    GAMENET_TEST_ASSERT(callbacksStayedOnOwner.load());
    GAMENET_TEST_ASSERT(
        stopResult.outcome == gamenet::net::TcpServerStopOutcome::ForcedAfterTimeout);
    GAMENET_TEST_ASSERT(stopResult.initialConnectionCount == 1);
    GAMENET_TEST_ASSERT(stopResult.forcedConnectionCount == 1);
    GAMENET_TEST_ASSERT(server.connectionCount() == 0);
}

void writeHeartbeat(
    std::uint64_t cycle,
    std::chrono::milliseconds elapsed,
    bool summary) {
    std::cout
        << "{\"schema\":\"gamenet.fault_injection_"
        << (summary ? "summary" : "heartbeat")
        << ".v1\",\"result\":\"pass\",\"cycle\":" << cycle
        << ",\"elapsed_milliseconds\":" << elapsed.count()
        << ",\"profiles\":{"
        << "\"abrupt_peer_reset\":" << cycle << ','
        << "\"callback_exception\":" << cycle << ','
        << "\"output_overload\":" << cycle << ','
        << "\"healthy_recovery\":" << cycle << ','
        << "\"forced_shutdown\":" << cycle
        << "}}\n"
        << std::flush;
}

void waitForSupervisorObservation() {
    const auto* required = std::getenv("GAMENET_ENDURANCE_OBSERVATION_ACK");
    if (required == nullptr || std::string_view(required) != "1") {
        return;
    }

    std::string acknowledgment;
    GAMENET_TEST_ASSERT(static_cast<bool>(std::getline(std::cin, acknowledgment)));
    GAMENET_TEST_ASSERT(acknowledgment == "observed");
}

}  // namespace

int main() {
    const auto targetDuration = environmentSeconds("GAMENET_ENDURANCE_SECONDS");
    const auto interval = environmentMilliseconds("GAMENET_ENDURANCE_INTERVAL_MS");
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t cycle = 0;

    do {
        runFaultCycle();
        ++cycle;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        writeHeartbeat(cycle, elapsed, false);
        waitForSupervisorObservation();

        if (targetDuration == 0s || elapsed >= targetDuration) {
            break;
        }
        if (interval > 0ms) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(targetDuration) - elapsed;
            std::this_thread::sleep_for(std::min(interval, remaining));
        }
    } while (std::chrono::steady_clock::now() - started < targetDuration);

    if (targetDuration > 0s) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed < targetDuration) {
            std::this_thread::sleep_for(targetDuration - elapsed);
        }
    }

    writeHeartbeat(
        cycle,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started),
        true);
    return 0;
}
