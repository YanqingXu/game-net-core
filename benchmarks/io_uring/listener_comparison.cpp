#include "experimental/io_uring/IoUringTcpConnectionHub.h"

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
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

constexpr std::size_t kActiveRoutes = 256;
constexpr std::size_t kMaxPendingAccepts = 32;
constexpr std::size_t kChurnWaves = 4;
constexpr std::size_t kReplacementsPerWave = 64;
constexpr std::size_t kRoundTripsPerRoutePerWave = 100;
constexpr std::size_t kPayloadBytes = 64;
constexpr std::size_t kExpectedAdmittedConnections =
    kActiveRoutes + kChurnWaves * kReplacementsPerWave;
constexpr std::size_t kExpectedCapacityRejections = kChurnWaves;
constexpr std::size_t kExpectedConnects =
    kExpectedAdmittedConnections + kExpectedCapacityRejections;
constexpr std::size_t kExpectedRoundTrips =
    (kChurnWaves + 1U) * kActiveRoutes * kRoundTripsPerRoutePerWave;

enum class Backend { Epoll, IoUring };

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

struct SharedObservation {
    std::atomic<std::size_t> activeRoutes{0};
    std::atomic<std::size_t> maxActiveRoutes{0};
    std::atomic<std::size_t> closeCompletions{0};
    std::atomic<std::size_t> capacityRejections{0};
    std::atomic<bool> loadDone{false};
};

struct WorkloadResult {
    std::vector<std::uint64_t> connectLatencyUs;
    std::vector<std::uint64_t> echoLatencyUs;
    std::vector<double> recoveryMilliseconds;
    double connectElapsedSeconds{};
    double echoElapsedSeconds{};
    std::size_t connectCompletions{};
    std::size_t roundTripCompletions{};
};

struct Sample {
    Backend backend{Backend::Epoll};
    WorkloadResult workload;
    std::uint64_t rssBefore{};
    std::uint64_t rssPeak{};
    std::uint64_t rssAfter{};
    std::size_t fdBefore{};
    std::size_t fdPeak{};
    std::size_t fdAfter{};
    std::size_t maxActiveRoutes{};
    std::optional<std::size_t> maxActiveAccepts;
    std::optional<std::size_t> maxActiveReceives;
    std::optional<std::size_t> maxActiveSends;
    std::optional<std::size_t> maxActiveOperations;
    std::size_t maxPendingSendBytes{};
    std::optional<std::size_t> maxEngineOwnedBytes;
    std::uint64_t capacityRejections{};
    std::optional<std::uint64_t> sqRejections;
    double listenerShutdownMilliseconds{};
    double serverShutdownMilliseconds{};
    std::size_t finalActiveRoutes{};
    std::optional<std::size_t> finalActiveAccepts;
    std::optional<std::size_t> finalActiveOperations;
    std::optional<std::size_t> finalReadyNotices;
    std::size_t finalPendingSendBytes{};
    std::optional<std::size_t> finalEngineOwnedBytes;
    bool listenerClosed{};
};

void updateMaximum(std::atomic<std::size_t>& target, std::size_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

std::uint64_t residentBytes() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        throw std::runtime_error("failed to read /proc/self/statm");
    }
    (void)totalPages;
    const auto pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) throw std::runtime_error("invalid page size");
    return residentPages * static_cast<std::uint64_t>(pageSize);
}

std::size_t openDescriptorCount() {
    DIR* raw = ::opendir("/proc/self/fd");
    if (raw == nullptr) throw std::runtime_error("failed to open /proc/self/fd");
    std::size_t count = 0;
    while (const auto* entry = ::readdir(raw)) {
        if (entry->d_name[0] == '.') continue;
        ++count;
    }
    ::closedir(raw);
    // The directory descriptor used for sampling is included above.
    return count - 1U;
}

std::uint64_t nearestRank(
    const std::vector<std::uint64_t>& sorted,
    double percentile) {
    if (sorted.empty()) throw std::runtime_error("empty latency sample");
    const auto rank = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(sorted.size())));
    return sorted[(std::clamp)(rank, std::size_t{1}, sorted.size()) - 1U];
}

ListenerEndpoint makeListener() {
    ListenerEndpoint endpoint{
        .socket = OwnedFd(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            IPPROTO_TCP)),
    };
    if (!endpoint.socket.valid()) throw std::runtime_error("socket failed");
    int enabled = 1;
    if (::setsockopt(
            endpoint.socket.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            sizeof(enabled)) != 0) {
        throw std::runtime_error("setsockopt failed");
    }
    endpoint.address.sin_family = AF_INET;
    endpoint.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.address.sin_port = 0;
    if (::bind(
            endpoint.socket.get(),
            reinterpret_cast<const sockaddr*>(&endpoint.address),
            sizeof(endpoint.address)) != 0 ||
        ::listen(endpoint.socket.get(), 512) != 0) {
        throw std::runtime_error("bind/listen failed");
    }
    socklen_t length = sizeof(endpoint.address);
    if (::getsockname(
            endpoint.socket.get(),
            reinterpret_cast<sockaddr*>(&endpoint.address),
            &length) != 0) {
        throw std::runtime_error("getsockname failed");
    }
    return endpoint;
}

OwnedFd connectClient(
    const sockaddr_in& address,
    std::vector<std::uint64_t>& latencyUs,
    double& elapsedSeconds) {
    OwnedFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (!client.valid()) throw std::runtime_error("client socket failed");
    const auto started = Clock::now();
    int result = 0;
    do {
        result = ::connect(
            client.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
    } while (result != 0 && errno == EINTR);
    const auto completed = Clock::now();
    if (result != 0) throw std::runtime_error("client connect failed");
    latencyUs.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(completed - started)
            .count()));
    elapsedSeconds += std::chrono::duration<double>(completed - started).count();
    const auto flags = ::fcntl(client.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("failed to make client nonblocking");
    }
    return client;
}

template <typename Predicate>
void waitUntil(Predicate predicate, std::string_view label) {
    const auto deadline = Clock::now() + 20s;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            throw std::runtime_error(std::string(label) + " timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void waitReady(int descriptor, short events) {
    pollfd item{.fd = descriptor, .events = events};
    while (true) {
        const auto result = ::poll(&item, 1, 20000);
        if (result > 0) return;
        if (result < 0 && errno == EINTR) continue;
        throw std::runtime_error("client poll timed out or failed");
    }
}

void sendExact(int descriptor, std::string_view payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto sent = ::send(
            descriptor,
            payload.data() + offset,
            payload.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            waitReady(descriptor, POLLOUT);
            continue;
        }
        throw std::runtime_error("client send failed");
    }
}

void waitForPeerClose(OwnedFd& socket) {
    std::array<char, 64> buffer{};
    const auto deadline = Clock::now() + 20s;
    while (true) {
        const auto received = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (received == 0) {
            socket.close();
            return;
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (Clock::now() >= deadline) {
                throw std::runtime_error("capacity rejection did not close peer");
            }
            pollfd item{.fd = socket.get(), .events = POLLIN | POLLHUP};
            const auto result = ::poll(&item, 1, 50);
            if (result < 0 && errno != EINTR) {
                throw std::runtime_error("capacity close poll failed");
            }
            continue;
        }
        if (received < 0 && (errno == ECONNRESET || errno == ENOTCONN)) {
            socket.close();
            return;
        }
        if (received > 0) {
            throw std::runtime_error("rejected connection received payload");
        }
        throw std::runtime_error("capacity rejection receive failed");
    }
}

void runEchoRounds(
    std::vector<OwnedFd>& clients,
    WorkloadResult& result) {
    const std::string payload(kPayloadBytes, 'x');
    std::vector<Clock::time_point> started(kActiveRoutes);
    std::vector<std::size_t> received(kActiveRoutes);
    std::array<char, kPayloadBytes> buffer{};
    const auto phaseStarted = Clock::now();
    for (std::size_t round = 0; round < kRoundTripsPerRoutePerWave; ++round) {
        std::vector<pollfd> descriptors;
        descriptors.reserve(kActiveRoutes);
        std::fill(received.begin(), received.end(), 0U);
        for (std::size_t index = 0; index < clients.size(); ++index) {
            if (!clients[index].valid()) {
                throw std::runtime_error("echo route is not connected");
            }
            started[index] = Clock::now();
            sendExact(clients[index].get(), payload);
            descriptors.push_back({
                .fd = clients[index].get(),
                .events = POLLIN,
            });
        }

        std::size_t remaining = kActiveRoutes;
        while (remaining != 0) {
            int pollResult = 0;
            do {
                pollResult = ::poll(descriptors.data(), descriptors.size(), 20000);
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0) throw std::runtime_error("echo poll failed");
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                auto& descriptor = descriptors[index];
                if (descriptor.fd < 0 || descriptor.revents == 0) continue;
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    throw std::runtime_error("echo route closed unexpectedly");
                }
                while (received[index] < kPayloadBytes) {
                    const auto count = ::recv(
                        descriptor.fd,
                        buffer.data(),
                        kPayloadBytes - received[index],
                        0);
                    if (count > 0) {
                        for (std::ptrdiff_t byte = 0; byte < count; ++byte) {
                            if (buffer[static_cast<std::size_t>(byte)] != 'x') {
                                throw std::runtime_error("echo payload mismatch");
                            }
                        }
                        received[index] += static_cast<std::size_t>(count);
                        continue;
                    }
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0 &&
                        (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    throw std::runtime_error("echo receive failed");
                }
                if (received[index] == kPayloadBytes) {
                    descriptor.fd = -1;
                    --remaining;
                    ++result.roundTripCompletions;
                    result.echoLatencyUs.push_back(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            Clock::now() - started[index])
                            .count()));
                }
            }
        }
    }
    result.echoElapsedSeconds +=
        std::chrono::duration<double>(Clock::now() - phaseStarted).count();
}

WorkloadResult runWorkload(
    sockaddr_in address,
    SharedObservation& observation) {
    WorkloadResult result;
    result.connectLatencyUs.reserve(kExpectedConnects);
    result.echoLatencyUs.reserve(kExpectedRoundTrips);
    result.recoveryMilliseconds.reserve(kChurnWaves);
    std::vector<OwnedFd> clients;
    clients.reserve(kActiveRoutes);
    for (std::size_t index = 0; index < kActiveRoutes; ++index) {
        clients.push_back(connectClient(
            address,
            result.connectLatencyUs,
            result.connectElapsedSeconds));
        ++result.connectCompletions;
    }
    waitUntil(
        [&] {
            return observation.activeRoutes.load(std::memory_order_relaxed) ==
                kActiveRoutes;
        },
        "initial route admission");
    runEchoRounds(clients, result);

    for (std::size_t wave = 0; wave < kChurnWaves; ++wave) {
        auto overflow = connectClient(
            address,
            result.connectLatencyUs,
            result.connectElapsedSeconds);
        ++result.connectCompletions;
        waitUntil(
            [&] {
                return observation.capacityRejections.load(
                           std::memory_order_relaxed) >= wave + 1U;
            },
            "capacity rejection");
        const auto rejectedAt = Clock::now();
        waitForPeerClose(overflow);

        const auto first = wave * kReplacementsPerWave;
        for (std::size_t index = first;
             index < first + kReplacementsPerWave;
             ++index) {
            clients[index].close();
        }
        waitUntil(
            [&] {
                return observation.activeRoutes.load(std::memory_order_relaxed) ==
                    kActiveRoutes - kReplacementsPerWave;
            },
            "route retirement");
        for (std::size_t index = first;
             index < first + kReplacementsPerWave;
             ++index) {
            clients[index] = connectClient(
                address,
                result.connectLatencyUs,
                result.connectElapsedSeconds);
            ++result.connectCompletions;
        }
        waitUntil(
            [&] {
                return observation.activeRoutes.load(std::memory_order_relaxed) ==
                    kActiveRoutes;
            },
            "capacity recovery");
        result.recoveryMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(
                Clock::now() - rejectedAt)
                .count());
        runEchoRounds(clients, result);
    }

    for (auto& client : clients) client.close();
    waitUntil(
        [&] {
            return observation.activeRoutes.load(std::memory_order_relaxed) == 0 &&
                observation.closeCompletions.load(std::memory_order_relaxed) ==
                    kExpectedAdmittedConnections;
        },
        "final route retirement");
    return result;
}

void sampleProcessHighWater(Sample& sample) {
    sample.fdPeak = (std::max)(sample.fdPeak, openDescriptorCount());
    sample.rssPeak = (std::max)(sample.rssPeak, residentBytes());
}

void validateWorkload(const Sample& sample) {
    if (sample.workload.connectCompletions != kExpectedConnects ||
        sample.workload.roundTripCompletions != kExpectedRoundTrips ||
        sample.workload.connectLatencyUs.size() != kExpectedConnects ||
        sample.workload.echoLatencyUs.size() != kExpectedRoundTrips ||
        sample.workload.recoveryMilliseconds.size() != kChurnWaves ||
        sample.maxActiveRoutes != kActiveRoutes ||
        sample.capacityRejections != kExpectedCapacityRejections ||
        sample.finalActiveRoutes != 0 || sample.finalPendingSendBytes != 0 ||
        !sample.listenerClosed) {
        throw std::runtime_error("listener benchmark accounting did not converge");
    }
}

void runEpollSample(Sample& sample) {
    SharedObservation observation;
    std::exception_ptr workerFailure;
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "ioe-x10-epoll-listener");
    server.setAdmissionOptions(
        gamenet::net::TcpServerAdmissionOptions{
            .maxConnections = kActiveRoutes,
        });
    server.setConnectionCallback(
        [&](const gamenet::net::TcpConnectionPtr& connection) {
            if (connection->connected()) {
                const auto active = observation.activeRoutes.fetch_add(
                                        1,
                                        std::memory_order_relaxed) +
                    1U;
                updateMaximum(observation.maxActiveRoutes, active);
            } else {
                observation.activeRoutes.fetch_sub(1, std::memory_order_relaxed);
                observation.closeCompletions.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        });
    server.setMessageCallback(
        [](const gamenet::net::TcpConnectionPtr& connection,
           gamenet::net::Buffer* input) {
            connection->send(input->retrieveAllAsString());
        });
    server.setAdmissionMetricCallback(
        [&](const gamenet::net::TcpServerAdmissionMetric& metric) {
            if (metric.event ==
                gamenet::net::TcpServerAdmissionEvent::RejectedConnectionLimit) {
                observation.capacityRejections.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        });
    server.start();

    std::thread worker([&] {
        try {
            sample.workload = runWorkload(
                server.listenAddress().getSockAddrInet(),
                observation);
        } catch (...) {
            workerFailure = std::current_exception();
        }
        observation.loadDone.store(true, std::memory_order_release);
    });

    gamenet::net::TcpServerStopFuture stopFuture;
    Clock::time_point stopStarted{};
    bool timedOut = false;
    const auto monitor = loop.runEvery(1ms, [&] {
        sampleProcessHighWater(sample);
        const auto output = server.outputMemoryStats();
        sample.maxPendingSendBytes = (std::max)(
            sample.maxPendingSendBytes,
            output.server.peakPendingBytes);
        if (!observation.loadDone.load(std::memory_order_acquire)) return;
        if (!stopFuture.valid()) {
            stopStarted = Clock::now();
            stopFuture = server.stopGracefully(
                gamenet::net::TcpServerStopOptions{.drainTimeout = 5s});
            return;
        }
        if (stopFuture.wait_for(0s) != std::future_status::ready) return;
        const auto stopResult = stopFuture.get();
        if (stopResult.outcome != gamenet::net::TcpServerStopOutcome::Drained) {
            workerFailure = std::make_exception_ptr(
                std::runtime_error("epoll server did not drain"));
        }
        sample.serverShutdownMilliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - stopStarted)
                .count();
        sample.listenerShutdownMilliseconds = sample.serverShutdownMilliseconds;
        sample.listenerClosed = true;
        loop.quit();
    });
    loop.runAfter(120s, [&] {
        timedOut = true;
        server.stop();
        loop.quit();
    });
    loop.loop();
    loop.cancel(monitor);
    if (worker.joinable()) worker.join();
    if (timedOut) throw std::runtime_error("epoll listener sample timed out");
    if (workerFailure) std::rethrow_exception(workerFailure);

    const auto admission = server.admissionStats();
    const auto output = server.outputMemoryStats();
    sample.maxActiveRoutes = observation.maxActiveRoutes.load(
        std::memory_order_relaxed);
    sample.capacityRejections = admission.rejectedConnectionLimit;
    sample.finalActiveRoutes = server.connectionCount();
    sample.finalPendingSendBytes = output.server.pendingBytes;
    validateWorkload(sample);
}

uring::IoUringTcpConnectionHubOptions hubOptions() {
    return {
        .pump = {
            .engine = {
                .entries = 1024,
                .maxOperations = 1024,
                .maxCompletionsPerWait = 1024,
                .maxBytesPerOperation = kPayloadBytes,
                .maxOwnedBytes = 2U * 1024U * 1024U,
            },
            .maxNoticesPerTurn = 512,
        },
        .maxConnections = kActiveRoutes,
        .maxTotalPendingSendBytes = kActiveRoutes * 4096U,
        .maxReceiveBytes = kPayloadBytes,
        .maxSendBytesPerOperation = kPayloadBytes,
        .maxPendingSendBytesPerConnection = 4096,
        .maxPendingSendSegmentsPerConnection = 64,
        .maxPendingAccepts = kMaxPendingAccepts,
    };
}

void runIoUringSample(Sample& sample) {
    SharedObservation observation;
    std::exception_ptr workerFailure;
    gamenet::net::EventLoop loop;
    std::optional<uring::IoUringTcpConnectionHubStopSummary> stopSummary;
    uring::IoUringTcpConnectionHub* hubPointer = nullptr;
    Clock::time_point serverStopStarted{};
    uring::IoUringTcpConnectionHub hub(
        &loop,
        hubOptions(),
        [&](const uring::IoUringTcpConnectionHubStopSummary& summary) {
            stopSummary = summary;
            sample.serverShutdownMilliseconds =
                std::chrono::duration<double, std::milli>(
                    Clock::now() - serverStopStarted)
                    .count();
            loop.quit();
        });
    hubPointer = &hub;
    auto endpoint = makeListener();
    const auto address = endpoint.address;
    const auto listenOutcome = hub.listen(endpoint.socket.release(), [&] {
        return uring::IoUringTcpHubAcceptedConnectionCallbacks{
            .messageConsumer =
                [&](uring::IoUringTcpConnectionIdentity identity,
                    std::string_view payload) {
                    if (hubPointer->send(identity, payload) !=
                        uring::IoUringTcpHubSendResult::Accepted) {
                        throw std::runtime_error("io_uring echo send rejected");
                    }
                },
            .closeConsumer =
                [&](uring::IoUringTcpConnectionIdentity,
                    uring::IoUringTcpHubCloseReason) {
                    observation.activeRoutes.fetch_sub(
                        1,
                        std::memory_order_relaxed);
                    observation.closeCompletions.fetch_add(
                        1,
                        std::memory_order_relaxed);
                },
        };
    });
    if (listenOutcome.result != uring::IoUringTcpHubListenResult::Accepted) {
        throw std::runtime_error("io_uring listener admission failed");
    }

    std::thread worker([&] {
        try {
            sample.workload = runWorkload(address, observation);
        } catch (...) {
            workerFailure = std::current_exception();
        }
        observation.loadDone.store(true, std::memory_order_release);
    });

    bool listenerStopRequested = false;
    bool hubStopRequested = false;
    bool timedOut = false;
    Clock::time_point listenerStopStarted{};
    const auto monitor = loop.runEvery(1ms, [&] {
        sampleProcessHighWater(sample);
        const auto metrics = hub.metrics();
        const auto listenerMetrics = hub.listenerMetrics();
        observation.activeRoutes.store(
            metrics.activeConnections,
            std::memory_order_relaxed);
        updateMaximum(observation.maxActiveRoutes, metrics.activeConnections);
        observation.capacityRejections.store(
            listenerMetrics.connectionLimitRejections,
            std::memory_order_relaxed);
        sample.maxPendingSendBytes = (std::max)(
            sample.maxPendingSendBytes,
            metrics.maxPendingSendBytes);
        if (!observation.loadDone.load(std::memory_order_acquire)) return;
        if (!listenerStopRequested) {
            listenerStopRequested = true;
            listenerStopStarted = Clock::now();
            if (!hub.stopListening()) {
                workerFailure = std::make_exception_ptr(
                    std::runtime_error("io_uring listener stop rejected"));
                serverStopStarted = Clock::now();
                hubStopRequested = true;
                (void)hub.stop();
            }
            return;
        }
        if (hubStopRequested ||
            listenOutcome.stopFuture.wait_for(0s) != std::future_status::ready) {
            return;
        }
        sample.listenerShutdownMilliseconds =
            std::chrono::duration<double, std::milli>(
                Clock::now() - listenerStopStarted)
                .count();
        serverStopStarted = Clock::now();
        hubStopRequested = true;
        if (!hub.stop()) {
            workerFailure = std::make_exception_ptr(
                std::runtime_error("io_uring Hub stop rejected"));
            loop.quit();
        }
    });
    loop.runAfter(120s, [&] {
        timedOut = true;
        serverStopStarted = Clock::now();
        (void)hub.stop();
        loop.quit();
    });
    loop.loop();
    loop.cancel(monitor);
    if (worker.joinable()) worker.join();
    if (timedOut) throw std::runtime_error("io_uring listener sample timed out");
    if (workerFailure) std::rethrow_exception(workerFailure);
    if (!stopSummary.has_value() || !stopSummary->listener.has_value()) {
        throw std::runtime_error("io_uring stop summary missing");
    }

    const auto& summary = *stopSummary;
    const auto& listener = *summary.listener;
    const auto expectedBytes = kExpectedRoundTrips * kPayloadBytes;
    if (!summary.allConnectionsStopped ||
        summary.hub.connectionsAccepted != kExpectedAdmittedConnections ||
        summary.hub.connectionsRetired != kExpectedAdmittedConnections ||
        summary.hub.bytesReceived != expectedBytes ||
        summary.hub.bytesSent != expectedBytes ||
        summary.hub.bytesDiscarded != 0 ||
        summary.hub.engineRejections != 0 ||
        summary.hub.invariantFailures != 0 ||
        listener.listener.connectionLimitRejections !=
            kExpectedCapacityRejections ||
        listener.listener.activeAccepts != 0 ||
        summary.pump.engine.sqFullRejections != 0 ||
        summary.pump.engine.operationLimitRejections != 0 ||
        summary.pump.engine.byteLimitRejections != 0 ||
        summary.pump.engine.crossDomainFallbacks != 0) {
        throw std::runtime_error("io_uring listener summary is inconsistent");
    }

    sample.maxActiveRoutes = summary.hub.maxActiveConnections;
    sample.maxActiveAccepts = listener.listener.maxActiveAccepts;
    sample.maxActiveReceives = summary.hub.maxActiveReceives;
    sample.maxActiveSends = summary.hub.maxActiveSends;
    sample.maxActiveOperations = summary.pump.engine.maxActiveOperations;
    sample.maxPendingSendBytes = summary.hub.maxPendingSendBytes;
    sample.maxEngineOwnedBytes = summary.pump.engine.maxOwnedBytes;
    sample.capacityRejections = listener.listener.connectionLimitRejections;
    sample.sqRejections = summary.pump.engine.sqFullRejections;
    sample.finalActiveRoutes = summary.hub.activeConnections;
    sample.finalActiveAccepts = listener.listener.activeAccepts;
    sample.finalActiveOperations = summary.pump.engine.activeOperations;
    sample.finalReadyNotices = summary.pump.engine.readyNotices;
    sample.finalPendingSendBytes = summary.hub.pendingSendBytes;
    sample.finalEngineOwnedBytes = summary.pump.engine.ownedBytes;
    sample.listenerClosed = listener.socketClosed && listener.acceptsRetired;
    validateWorkload(sample);
}

std::string_view backendName(Backend backend) {
    return backend == Backend::Epoll ? "epoll" : "io_uring";
}

template <typename Value>
void printOptional(std::optional<Value> value) {
    if (value.has_value()) {
        std::cout << *value;
    } else {
        std::cout << "null";
    }
}

void printSample(Sample sample) {
    std::sort(
        sample.workload.connectLatencyUs.begin(),
        sample.workload.connectLatencyUs.end());
    std::sort(
        sample.workload.echoLatencyUs.begin(),
        sample.workload.echoLatencyUs.end());
    const auto maximumRecovery = *std::max_element(
        sample.workload.recoveryMilliseconds.begin(),
        sample.workload.recoveryMilliseconds.end());
    const auto connectsPerSecond =
        static_cast<double>(kExpectedConnects) /
        sample.workload.connectElapsedSeconds;
    const auto roundTripsPerSecond =
        static_cast<double>(kExpectedRoundTrips) /
        sample.workload.echoElapsedSeconds;
    const auto throughputMiBPerSecond =
        static_cast<double>(2U * kExpectedRoundTrips * kPayloadBytes) /
        (1024.0 * 1024.0 * sample.workload.echoElapsedSeconds);

    std::cout << std::fixed << std::setprecision(9)
              << "{\n"
              << "  \"schema\": \"gamenet.ioe_x10_listener_comparison.v1\",\n"
              << "  \"status\": \"ok\",\n"
              << "  \"backend\": \"" << backendName(sample.backend) << "\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE
              << "\",\n"
              << "  \"parameters\": {\"active_routes\": " << kActiveRoutes
              << ", \"max_pending_accepts\": " << kMaxPendingAccepts
              << ", \"hub_route_limit\": " << kActiveRoutes
              << ", \"churn_waves\": " << kChurnWaves
              << ", \"replacements_per_wave\": " << kReplacementsPerWave
              << ", \"round_trips_per_route_per_wave\": "
              << kRoundTripsPerRoutePerWave
              << ", \"payload_bytes\": " << kPayloadBytes << "},\n"
              << "  \"measurements\": {\n"
              << "    \"connect_completions\": " << kExpectedConnects << ",\n"
              << "    \"connections_admitted\": "
              << kExpectedAdmittedConnections << ",\n"
              << "    \"echo_completions\": " << kExpectedRoundTrips << ",\n"
              << "    \"close_completions\": "
              << kExpectedAdmittedConnections << ",\n"
              << "    \"connect_completion_rate\": 1.000000000,\n"
              << "    \"echo_completion_rate\": 1.000000000,\n"
              << "    \"close_completion_rate\": 1.000000000,\n"
              << "    \"connects_per_second\": " << connectsPerSecond << ",\n"
              << "    \"round_trips_per_second\": " << roundTripsPerSecond
              << ",\n"
              << "    \"throughput_mib_per_second\": "
              << throughputMiBPerSecond << ",\n"
              << "    \"p50_latency_us\": "
              << nearestRank(sample.workload.echoLatencyUs, 0.50) << ",\n"
              << "    \"p99_latency_us\": "
              << nearestRank(sample.workload.echoLatencyUs, 0.99) << ",\n"
              << "    \"p999_latency_us\": "
              << nearestRank(sample.workload.echoLatencyUs, 0.999) << ",\n"
              << "    \"fd_baseline\": " << sample.fdBefore << ",\n"
              << "    \"fd_high_water\": " << sample.fdPeak << ",\n"
              << "    \"fd_final\": " << sample.fdAfter << ",\n"
              << "    \"fd_residue\": " << (sample.fdAfter - sample.fdBefore)
              << ",\n"
              << "    \"max_active_routes\": " << sample.maxActiveRoutes
              << ",\n"
              << "    \"max_active_accepts\": ";
    printOptional(sample.maxActiveAccepts);
    std::cout << ",\n    \"max_active_recvs\": ";
    printOptional(sample.maxActiveReceives);
    std::cout << ",\n    \"max_active_sends\": ";
    printOptional(sample.maxActiveSends);
    std::cout << ",\n    \"max_active_operations\": ";
    printOptional(sample.maxActiveOperations);
    std::cout << ",\n"
              << "    \"max_pending_send_bytes\": "
              << sample.maxPendingSendBytes << ",\n"
              << "    \"max_engine_owned_bytes\": ";
    printOptional(sample.maxEngineOwnedBytes);
    std::cout << ",\n"
              << "    \"rss_before_bytes\": " << sample.rssBefore << ",\n"
              << "    \"rss_high_water_bytes\": " << sample.rssPeak << ",\n"
              << "    \"rss_after_bytes\": " << sample.rssAfter << ",\n"
              << "    \"capacity_rejections\": "
              << sample.capacityRejections << ",\n"
              << "    \"sq_rejections\": ";
    printOptional(sample.sqRejections);
    std::cout << ",\n"
              << "    \"capacity_recovered\": true,\n"
              << "    \"max_capacity_recovery_milliseconds\": "
              << maximumRecovery << ",\n"
              << "    \"listener_shutdown_milliseconds\": "
              << sample.listenerShutdownMilliseconds << ",\n"
              << "    \"server_shutdown_milliseconds\": "
              << sample.serverShutdownMilliseconds << ",\n"
              << "    \"listener_closed\": true,\n"
              << "    \"final_active_routes\": " << sample.finalActiveRoutes
              << ",\n"
              << "    \"final_active_accepts\": ";
    printOptional(sample.finalActiveAccepts);
    std::cout << ",\n    \"final_active_operations\": ";
    printOptional(sample.finalActiveOperations);
    std::cout << ",\n    \"final_ready_notices\": ";
    printOptional(sample.finalReadyNotices);
    std::cout << ",\n"
              << "    \"final_pending_send_bytes\": "
              << sample.finalPendingSendBytes << ",\n"
              << "    \"final_engine_owned_bytes\": ";
    printOptional(sample.finalEngineOwnedBytes);
    std::cout << "\n  }\n}\n";
}

Backend parseBackend(int argc, char* argv[]) {
    if (argc != 2) {
        throw std::invalid_argument(
            "usage: gamenet_ioe_x10_listener_comparison_benchmark "
            "<epoll|io_uring>");
    }
    const std::string_view value(argv[1]);
    if (value == "epoll") return Backend::Epoll;
    if (value == "io_uring") return Backend::IoUring;
    throw std::invalid_argument("backend must be epoll or io_uring");
}

int run(Backend backend) {
    Sample sample{.backend = backend};
    sample.fdBefore = openDescriptorCount();
    sample.fdPeak = sample.fdBefore;
    sample.rssBefore = residentBytes();
    sample.rssPeak = sample.rssBefore;
    if (backend == Backend::Epoll) {
        runEpollSample(sample);
    } else {
        runIoUringSample(sample);
    }
    sample.fdAfter = openDescriptorCount();
    sample.rssAfter = residentBytes();
    sample.rssPeak = (std::max)(sample.rssPeak, sample.rssAfter);
    if (sample.fdAfter != sample.fdBefore) {
        throw std::runtime_error("listener benchmark leaked descriptors");
    }
    printSample(std::move(sample));
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseBackend(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "IOE-X10 listener comparison failed: " << error.what()
                  << '\n';
        return 1;
    }
}
