#include "experimental/io_uring/IoUringTcpConnectionHub.h"

#include "gamenet/core/net/EventLoop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

namespace uring = gamenet::experimental::io_uring;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Config {
    std::size_t connections{256};
    std::size_t roundTripsPerConnection{100};
    std::size_t payloadBytes{64};
};

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

struct SocketPair {
    OwnedFd hub;
    OwnedFd peer;
};

class TcpPairFactory {
public:
    explicit TcpPairFactory(std::size_t backlog)
        : listener_(::socket(
              AF_INET,
              SOCK_STREAM | SOCK_CLOEXEC,
              IPPROTO_TCP)) {
        if (listener_.get() < 0) throw std::runtime_error("socket failed");
        int enabled = 1;
        if (::setsockopt(
                listener_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &enabled,
                sizeof(enabled)) != 0) {
            throw std::runtime_error("setsockopt failed");
        }
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_.sin_port = 0;
        if (::bind(
                listener_.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) != 0 ||
            ::listen(listener_.get(), static_cast<int>(backlog)) != 0) {
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t length = sizeof(address_);
        if (::getsockname(
                listener_.get(),
                reinterpret_cast<sockaddr*>(&address_),
                &length) != 0) {
            throw std::runtime_error("getsockname failed");
        }
    }

    SocketPair makePair() {
        OwnedFd peer(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            IPPROTO_TCP));
        if (peer.get() < 0 ||
            ::connect(
                peer.get(),
                reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) != 0) {
            throw std::runtime_error("connect failed");
        }
        OwnedFd hub(::accept4(
            listener_.get(),
            nullptr,
            nullptr,
            SOCK_CLOEXEC));
        if (hub.get() < 0) throw std::runtime_error("accept4 failed");
        return {.hub = std::move(hub), .peer = std::move(peer)};
    }

private:
    OwnedFd listener_;
    sockaddr_in address_{};
};

struct RouteState {
    uring::IoUringTcpConnectionIdentity identity{};
    std::shared_future<uring::IoUringTcpConnectionHubConnectionStopSummary>
        stopFuture;
    Clock::time_point startedAt{};
    std::size_t partialBytes{};
    std::size_t completed{};
};

std::size_t parsePositive(
    const char* text,
    std::string_view name,
    std::size_t maximum) {
    try {
        const std::string value(text);
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > maximum) {
            throw std::invalid_argument("out of range");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string(name) + " must be a positive bounded integer");
    }
}

Config parseConfig(int argc, char* argv[]) {
    if (argc > 4) {
        throw std::invalid_argument(
            "usage: gamenet_io_uring_shared_tcp_hub_benchmark "
            "[connections] [round-trips-per-connection] [payload-bytes]");
    }
    Config config;
    if (argc > 1) {
        config.connections = parsePositive(argv[1], "connections", 1024);
    }
    if (argc > 2) {
        config.roundTripsPerConnection =
            parsePositive(argv[2], "round-trips-per-connection", 10000);
    }
    if (argc > 3) {
        config.payloadBytes = parsePositive(argv[3], "payload-bytes", 4096);
    }
    if (config.connections < 2) {
        throw std::invalid_argument("connections must be at least two");
    }
    if (config.connections >
            (std::numeric_limits<std::size_t>::max)() /
                config.roundTripsPerConnection ||
        config.connections * config.roundTripsPerConnection > 10000000) {
        throw std::invalid_argument(
            "total round trips exceed the finite ten-million bound");
    }
    if (config.connections >
        (std::numeric_limits<std::size_t>::max)() /
            (2U * config.payloadBytes)) {
        throw std::invalid_argument("connection byte budgets overflow size_t");
    }
    return config;
}

std::uint64_t workingSetBytes() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        throw std::runtime_error("failed to read /proc/self/statm");
    }
    (void)totalPages;
    const auto pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");
    return residentPages * static_cast<std::uint64_t>(pageSize);
}

std::int64_t signedDelta(std::uint64_t after, std::uint64_t before) noexcept {
    if (after >= before) {
        const auto delta = after - before;
        return delta > static_cast<std::uint64_t>(
                           (std::numeric_limits<std::int64_t>::max)())
            ? (std::numeric_limits<std::int64_t>::max)()
            : static_cast<std::int64_t>(delta);
    }
    const auto delta = before - after;
    return delta > static_cast<std::uint64_t>(
                       (std::numeric_limits<std::int64_t>::max)())
        ? (std::numeric_limits<std::int64_t>::min)()
        : -static_cast<std::int64_t>(delta);
}

std::uint64_t nearestRank(
    const std::vector<std::uint64_t>& sorted,
    double percentile) {
    const auto rank = static_cast<std::size_t>(
        percentile * static_cast<double>(sorted.size()) + 0.999999999);
    return sorted[(std::clamp)(rank, std::size_t{1}, sorted.size()) - 1U];
}

void echoPeers(
    const std::vector<SocketPair>& pairs,
    std::atomic<bool>& failed) noexcept {
    std::vector<pollfd> descriptors;
    descriptors.reserve(pairs.size());
    for (const auto& pair : pairs) {
        descriptors.push_back({.fd = pair.peer.get(), .events = POLLIN});
    }
    std::vector<char> bytes(4096);
    std::size_t open = descriptors.size();
    while (open != 0) {
        int result = 0;
        do {
            result = ::poll(descriptors.data(), descriptors.size(), 1000);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            failed.store(true, std::memory_order_relaxed);
            return;
        }
        if (result == 0) continue;
        for (auto& descriptor : descriptors) {
            if (descriptor.fd < 0 || descriptor.revents == 0) continue;
            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            const auto received =
                ::recv(descriptor.fd, bytes.data(), bytes.size(), 0);
            if (received == 0) {
                descriptor.fd = -1;
                --open;
                continue;
            }
            if (received < 0) {
                if (errno == EINTR) continue;
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            std::size_t sent = 0;
            while (sent < static_cast<std::size_t>(received)) {
                const auto count = ::send(
                    descriptor.fd,
                    bytes.data() + sent,
                    static_cast<std::size_t>(received) - sent,
                    MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                sent += static_cast<std::size_t>(count);
            }
        }
    }
}

int run(const Config& config) {
    const auto workingSetBefore = workingSetBytes();
    TcpPairFactory factory(config.connections);
    std::vector<SocketPair> pairs;
    pairs.reserve(config.connections);
    for (std::size_t index = 0; index < config.connections; ++index) {
        pairs.push_back(factory.makePair());
    }

    gamenet::net::EventLoop loop;
    std::vector<RouteState> routes(config.connections);
    const auto totalRoundTrips =
        config.connections * config.roundTripsPerConnection;
    std::vector<std::uint64_t> latencyUs;
    latencyUs.reserve(totalRoundTrips);
    std::vector<char> payload(config.payloadBytes, 'h');
    std::size_t completed = 0;
    std::size_t closeCallbacks = 0;
    bool timedOut = false;
    bool ownerFailure = false;
    Clock::time_point dataCompletedAt{};
    Clock::time_point shutdownStartedAt{};
    double shutdownMilliseconds = 0;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> stoppedSummary;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;

    uring::IoUringTcpConnectionHub hub(
        &loop,
        uring::IoUringTcpConnectionHubOptions{
            .pump = {
                .engine = {
                    .entries = (std::min)(
                        std::size_t{4096},
                        (std::max)(std::size_t{64}, config.connections * 2U)),
                    .maxOperations = config.connections * 2U,
                    .maxCompletionsPerWait = config.connections * 2U,
                    .maxBytesPerOperation = config.payloadBytes,
                    .maxOwnedBytes =
                        config.connections * config.payloadBytes * 2U,
                },
                .maxNoticesPerTurn = config.connections,
            },
            .maxConnections = config.connections,
            .maxTotalPendingSendBytes =
                config.connections * config.payloadBytes,
            .maxReceiveBytes = config.payloadBytes,
            .maxSendBytesPerOperation = config.payloadBytes,
            .maxPendingSendBytesPerConnection = config.payloadBytes,
            .maxPendingSendSegmentsPerConnection = 1,
        },
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            shutdownMilliseconds =
                std::chrono::duration<double, std::milli>(
                    Clock::now() - shutdownStartedAt)
                    .count();
            stoppedSummary = summary;
            loop.quit();
        });
    hubPointer = &hub;

    for (std::size_t index = 0; index < config.connections; ++index) {
        const auto added = hub.addConnection(
            pairs[index].hub.release(),
            [&, index](
                uring::IoUringTcpConnectionIdentity identity,
                std::string_view bytes) {
                auto& route = routes[index];
                if (identity != route.identity || bytes.empty() ||
                    bytes.size() > config.payloadBytes - route.partialBytes) {
                    ownerFailure = true;
                    (void)hubPointer->stop();
                    return;
                }
                route.partialBytes += bytes.size();
                if (route.partialBytes != config.payloadBytes) return;

                route.partialBytes = 0;
                latencyUs.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - route.startedAt)
                        .count()));
                ++route.completed;
                ++completed;
                if (route.completed < config.roundTripsPerConnection) {
                    route.startedAt = Clock::now();
                    if (hubPointer->send(
                            identity,
                            std::string_view(payload.data(), payload.size())) !=
                        uring::IoUringTcpHubSendResult::Accepted) {
                        ownerFailure = true;
                        (void)hubPointer->stop();
                    }
                    return;
                }
                if (completed == totalRoundTrips) {
                    dataCompletedAt = Clock::now();
                    shutdownStartedAt = dataCompletedAt;
                    if (!hubPointer->stop()) ownerFailure = true;
                }
            },
            [&](uring::IoUringTcpConnectionIdentity,
                uring::IoUringTcpHubCloseReason reason) {
                if (reason != uring::IoUringTcpHubCloseReason::HubStopped) {
                    ownerFailure = true;
                }
                ++closeCallbacks;
            });
        if (added.result != uring::IoUringTcpHubAddResult::Accepted) {
            throw std::runtime_error("shared-Hub connection admission failed");
        }
        routes[index].identity = added.identity;
        routes[index].stopFuture = added.stopFuture;
    }

    std::atomic<bool> peerFailed{false};
    std::thread peer([&] { echoPeers(pairs, peerFailed); });
    const auto startedAt = Clock::now();
    for (auto& route : routes) {
        route.startedAt = Clock::now();
        if (hub.send(
                route.identity,
                std::string_view(payload.data(), payload.size())) !=
            uring::IoUringTcpHubSendResult::Accepted) {
            throw std::runtime_error("initial shared-Hub send rejected");
        }
    }
    const auto workingSetActive = workingSetBytes();
    loop.runAfter(60s, [&] {
        timedOut = true;
        shutdownStartedAt = Clock::now();
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();
    if (peer.joinable()) peer.join();
    const auto workingSetAfter = workingSetBytes();

    if (timedOut || ownerFailure || peerFailed.load(std::memory_order_relaxed) ||
        completed != totalRoundTrips || closeCallbacks != config.connections ||
        !stoppedSummary.has_value()) {
        throw std::runtime_error("shared-Hub benchmark did not converge");
    }
    for (const auto& route : routes) {
        if (route.completed != config.roundTripsPerConnection ||
            route.partialBytes != 0 ||
            route.stopFuture.wait_for(0s) != std::future_status::ready) {
            throw std::runtime_error("shared-Hub route did not retire exactly");
        }
    }

    const auto& summary = *stoppedSummary;
    const auto expectedBytes = totalRoundTrips * config.payloadBytes;
    if (!summary.allConnectionsStopped ||
        summary.hub.connectionsAccepted != config.connections ||
        summary.hub.connectionsRetired != config.connections ||
        summary.hub.maxActiveConnections != config.connections ||
        summary.hub.sendAdmissions != totalRoundTrips ||
        summary.hub.bytesSent != expectedBytes ||
        summary.hub.bytesDiscarded != 0 ||
        summary.hub.bytesReceived != expectedBytes ||
        summary.hub.activeConnections != 0 ||
        summary.hub.activeOperationRoutes != 0 ||
        summary.hub.pendingSendBytes != 0 ||
        summary.pump.engine.activeOperations != 0 ||
        summary.pump.engine.readyNotices != 0 ||
        summary.pump.engine.ownedBytes != 0 ||
        summary.pump.engine.crossDomainFallbacks != 0) {
        throw std::runtime_error("shared-Hub benchmark accounting did not converge");
    }

    std::sort(latencyUs.begin(), latencyUs.end());
    const auto elapsedSeconds =
        std::chrono::duration<double>(dataCompletedAt - startedAt).count();
    const auto roundTripsPerSecond =
        static_cast<double>(totalRoundTrips) / elapsedSeconds;
    const auto throughputMiBPerSecond =
        static_cast<double>(2U * expectedBytes) /
        (1024.0 * 1024.0 * elapsedSeconds);
    const auto activeDelta = signedDelta(workingSetActive, workingSetBefore);
    const auto bytesPerConnection = activeDelta <= 0
        ? 0.0
        : static_cast<double>(activeDelta) /
            static_cast<double>(config.connections);

    std::cout << std::fixed << std::setprecision(9)
              << "{\n"
              << "  \"schema\": \"gamenet.io_uring_shared_tcp_hub_benchmark.v1\",\n"
              << "  \"status\": \"ok\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE
              << "\",\n"
              << "  \"parameters\": {\"connections\": "
              << config.connections
              << ", \"round_trips_per_connection\": "
              << config.roundTripsPerConnection
              << ", \"payload_bytes\": " << config.payloadBytes << "},\n"
              << "  \"measurements\": {\n"
              << "    \"elapsed_seconds\": " << elapsedSeconds << ",\n"
              << "    \"completed_round_trips\": " << totalRoundTrips << ",\n"
              << "    \"round_trips_per_second\": " << roundTripsPerSecond
              << ",\n"
              << "    \"throughput_mib_per_second\": "
              << throughputMiBPerSecond << ",\n"
              << "    \"p50_latency_us\": "
              << nearestRank(latencyUs, 0.50) << ",\n"
              << "    \"p99_latency_us\": "
              << nearestRank(latencyUs, 0.99) << ",\n"
              << "    \"p999_latency_us\": "
              << nearestRank(latencyUs, 0.999) << ",\n"
              << "    \"working_set_before_bytes\": " << workingSetBefore
              << ",\n"
              << "    \"working_set_active_bytes\": " << workingSetActive
              << ",\n"
              << "    \"working_set_after_bytes\": " << workingSetAfter
              << ",\n"
              << "    \"working_set_active_delta_bytes\": " << activeDelta
              << ",\n"
              << "    \"working_set_bytes_per_connection\": "
              << bytesPerConnection << ",\n"
              << "    \"shutdown_milliseconds\": " << shutdownMilliseconds
              << ",\n"
              << "    \"connections_accepted\": "
              << summary.hub.connectionsAccepted << ",\n"
              << "    \"connections_retired\": "
              << summary.hub.connectionsRetired << ",\n"
              << "    \"max_active_connections\": "
              << summary.hub.maxActiveConnections << ",\n"
              << "    \"send_admissions\": "
              << summary.hub.sendAdmissions << ",\n"
              << "    \"bytes_sent\": " << summary.hub.bytesSent << ",\n"
              << "    \"bytes_received\": " << summary.hub.bytesReceived
              << ",\n"
              << "    \"bytes_discarded\": "
              << summary.hub.bytesDiscarded << ",\n"
              << "    \"engine_rejections\": "
              << summary.hub.engineRejections << ",\n"
              << "    \"cross_domain_fallbacks\": "
              << summary.pump.engine.crossDomainFallbacks << ",\n"
              << "    \"active_operation_routes\": "
              << summary.hub.activeOperationRoutes << ",\n"
              << "    \"pending_send_bytes\": "
              << summary.hub.pendingSendBytes << ",\n"
              << "    \"engine_active_operations\": "
              << summary.pump.engine.activeOperations << ",\n"
              << "    \"engine_ready_notices\": "
              << summary.pump.engine.readyNotices << ",\n"
              << "    \"engine_owned_bytes\": "
              << summary.pump.engine.ownedBytes << "\n"
              << "  }\n"
              << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseConfig(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "io_uring shared-Hub benchmark failed: "
                  << error.what() << '\n';
        return 1;
    }
}
