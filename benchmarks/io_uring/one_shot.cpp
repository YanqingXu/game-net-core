#include "experimental/io_uring/IoUringCompletionEngine.h"

#include "gamenet/core/net/EventLoop.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
    std::size_t roundTrips{100000};
    std::size_t payloadBytes{256};
    std::size_t depth{64};
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

private:
    int value_;
};

struct PendingOperation {
    Clock::time_point startedAt;
    uring::IoUringOperationKind kind;
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
            "usage: gamenet_io_uring_one_shot_benchmark "
            "[round-trips] [payload-bytes] [depth]");
    }
    Config config;
    if (argc > 1) {
        config.roundTrips = parsePositive(argv[1], "round-trips", 10000000);
    }
    if (argc > 2) {
        config.payloadBytes = parsePositive(argv[2], "payload-bytes", 65536);
    }
    if (argc > 3) {
        config.depth = parsePositive(argv[3], "depth", 1024);
    }
    config.depth = std::min(config.depth, config.roundTrips);
    if (config.depth >
        std::numeric_limits<std::size_t>::max() / config.payloadBytes) {
        throw std::invalid_argument("depth times payload overflows size_t");
    }
    return config;
}

std::uint64_t identityKey(uring::IoUringOperationIdentity identity) noexcept {
    return (static_cast<std::uint64_t>(identity.generation) << 32U) |
        identity.slot;
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

std::uint64_t nearestRank(
    const std::vector<std::uint64_t>& sorted,
    double percentile) {
    const auto rank = static_cast<std::size_t>(
        percentile * static_cast<double>(sorted.size()) + 0.999999999);
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

std::int64_t signedDelta(std::uint64_t after, std::uint64_t before) noexcept {
    if (after >= before) {
        const auto delta = after - before;
        return delta > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(delta);
    }
    const auto delta = before - after;
    return delta > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::min()
        : -static_cast<std::int64_t>(delta);
}

void requireAccepted(
    const uring::IoUringSubmissionOutcome& outcome,
    std::string_view operation) {
    if (outcome.result != uring::IoUringSubmissionResult::Accepted) {
        throw std::runtime_error(std::string(operation) + " submission rejected");
    }
}

int run(const Config& config) {
    std::array<int, 2> descriptors{};
    if (::socketpair(
            AF_UNIX,
            SOCK_SEQPACKET | SOCK_CLOEXEC,
            0,
            descriptors.data()) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    OwnedFd engineSocket(descriptors[0]);
    OwnedFd peerSocket(descriptors[1]);

    gamenet::net::EventLoop ownerLoop;
    uring::IoUringCompletionEngine engine(
        &ownerLoop,
        uring::IoUringCompletionEngineOptions{
            .entries = config.depth,
            .maxOperations = config.depth,
            .maxCompletionsPerWait = config.depth,
            .maxBytesPerOperation = config.payloadBytes,
            .maxOwnedBytes = config.depth * config.payloadBytes,
        });

    std::atomic<bool> peerFailed{false};
    std::jthread peer([&](std::stop_token stop) {
        std::vector<char> bytes(config.payloadBytes);
        for (std::size_t index = 0;
             index < config.roundTrips && !stop.stop_requested();
             ++index) {
            const auto received = ::recv(
                peerSocket.get(), bytes.data(), bytes.size(), 0);
            if (received != static_cast<ssize_t>(bytes.size())) {
                if (!stop.stop_requested()) peerFailed.store(true);
                return;
            }
            const auto sent = ::send(
                peerSocket.get(),
                bytes.data(),
                bytes.size(),
                MSG_NOSIGNAL);
            if (sent != static_cast<ssize_t>(bytes.size())) {
                if (!stop.stop_requested()) peerFailed.store(true);
                return;
            }
        }
    });

    std::vector<char> payload(config.payloadBytes, 'u');
    std::unordered_map<std::uint64_t, PendingOperation> pending;
    pending.reserve(config.depth * 2U);
    std::vector<std::uint64_t> latencyUs;
    latencyUs.reserve(config.roundTrips);
    std::size_t scheduled = 0;
    std::size_t completed = 0;
    const auto workingSetBefore = workingSetBytes();
    const auto startedAt = Clock::now();
    const auto deadline = startedAt + 60s;

    auto enqueueSend = [&] {
        const auto submittedAt = Clock::now();
        const auto outcome = engine.enqueueSend(
            engineSocket.get(),
            std::string_view(payload.data(), payload.size()));
        requireAccepted(outcome, "Send");
        pending.emplace(
            identityKey(outcome.identity),
            PendingOperation{
                .startedAt = submittedAt,
                .kind = uring::IoUringOperationKind::Send,
            });
        ++scheduled;
    };

    try {
        for (std::size_t index = 0; index < config.depth; ++index) {
            enqueueSend();
        }
        const auto initialFlush = engine.flush();
        if (initialFlush.nativeError != 0 || initialFlush.pending != 0) {
            throw std::runtime_error("initial io_uring flush failed");
        }

        while (completed < config.roundTrips) {
            if (Clock::now() >= deadline || peerFailed.load()) {
                throw std::runtime_error("io_uring benchmark did not converge");
            }
            (void)engine.wait(100ms);
            while (auto notice = engine.takeNextNotice()) {
                const auto entry = pending.find(identityKey(notice->identity()));
                if (entry == pending.end() || entry->second.kind != notice->kind()) {
                    throw std::runtime_error("unexpected io_uring terminal identity");
                }
                if (notice->status() != uring::IoUringCompletionStatus::Succeeded ||
                    notice->bytesTransferred() != config.payloadBytes) {
                    throw std::runtime_error("io_uring operation failed or was partial");
                }
                const auto operation = entry->second;
                pending.erase(entry);

                if (notice->kind() == uring::IoUringOperationKind::Send) {
                    const auto outcome = engine.enqueueRecv(
                        engineSocket.get(), config.payloadBytes);
                    requireAccepted(outcome, "Receive");
                    pending.emplace(
                        identityKey(outcome.identity),
                        PendingOperation{
                            .startedAt = operation.startedAt,
                            .kind = uring::IoUringOperationKind::Receive,
                        });
                } else if (notice->kind() == uring::IoUringOperationKind::Receive) {
                    const auto elapsed = std::chrono::duration_cast<
                        std::chrono::microseconds>(Clock::now() - operation.startedAt);
                    latencyUs.push_back(static_cast<std::uint64_t>(elapsed.count()));
                    ++completed;
                    if (scheduled < config.roundTrips) enqueueSend();
                } else {
                    throw std::runtime_error("Accept notice in Send/Recv benchmark");
                }
            }
        }
    } catch (...) {
        peer.request_stop();
        ::shutdown(peerSocket.get(), SHUT_RDWR);
        if (peer.joinable()) peer.join();
        throw;
    }

    const auto completedAt = Clock::now();
    if (!pending.empty()) throw std::runtime_error("pending operation map is not empty");
    if (peer.joinable()) peer.join();
    if (peerFailed.load()) throw std::runtime_error("benchmark peer failed");

    const auto shutdownStartedAt = Clock::now();
    if (engine.shutdown(5s) != uring::IoUringShutdownResult::Drained) {
        throw std::runtime_error("io_uring benchmark shutdown did not drain");
    }
    const auto shutdownElapsed = std::chrono::duration<double, std::milli>(
        Clock::now() - shutdownStartedAt).count();
    const auto metrics = engine.metrics();
    if (metrics.operationsAccepted != 2U * config.roundTrips ||
        metrics.terminalNotices != 2U * config.roundTrips ||
        metrics.activeOperations != 0 || metrics.pendingSubmissions != 0 ||
        metrics.pendingCancelCompletions != 0 || metrics.readyNotices != 0 ||
        metrics.ownedBytes != 0 || metrics.crossDomainFallbacks != 0) {
        throw std::runtime_error("io_uring benchmark accounting did not converge");
    }

    std::sort(latencyUs.begin(), latencyUs.end());
    const auto workingSetAfter = workingSetBytes();
    const auto elapsedSeconds =
        std::chrono::duration<double>(completedAt - startedAt).count();
    const auto messagesPerSecond =
        static_cast<double>(config.roundTrips) / elapsedSeconds;
    const auto operationsPerSecond =
        static_cast<double>(2U * config.roundTrips) / elapsedSeconds;
    const auto throughputMiBPerSecond =
        static_cast<double>(2U * config.roundTrips * config.payloadBytes) /
        (1024.0 * 1024.0 * elapsedSeconds);

    std::cout << std::fixed << std::setprecision(9)
              << "{\n"
              << "  \"schema\": \"gamenet.io_uring_one_shot_benchmark.v1\",\n"
              << "  \"status\": \"ok\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"parameters\": {\"round_trips\": " << config.roundTrips
              << ", \"payload_bytes\": " << config.payloadBytes
              << ", \"depth\": " << config.depth << "},\n"
              << "  \"measurements\": {\n"
              << "    \"elapsed_seconds\": " << elapsedSeconds << ",\n"
              << "    \"messages_per_second\": " << messagesPerSecond << ",\n"
              << "    \"operations_per_second\": " << operationsPerSecond << ",\n"
              << "    \"throughput_mib_per_second\": " << throughputMiBPerSecond << ",\n"
              << "    \"p50_latency_us\": " << nearestRank(latencyUs, 0.50) << ",\n"
              << "    \"p99_latency_us\": " << nearestRank(latencyUs, 0.99) << ",\n"
              << "    \"p999_latency_us\": " << nearestRank(latencyUs, 0.999) << ",\n"
              << "    \"working_set_before_bytes\": " << workingSetBefore << ",\n"
              << "    \"working_set_after_bytes\": " << workingSetAfter << ",\n"
              << "    \"working_set_delta_bytes\": "
              << signedDelta(workingSetAfter, workingSetBefore) << ",\n"
              << "    \"shutdown_milliseconds\": " << shutdownElapsed << ",\n"
              << "    \"operations_accepted\": " << metrics.operationsAccepted << ",\n"
              << "    \"terminal_notices\": " << metrics.terminalNotices << ",\n"
              << "    \"sq_full_rejections\": " << metrics.sqFullRejections << ",\n"
              << "    \"cross_domain_fallbacks\": " << metrics.crossDomainFallbacks << ",\n"
              << "    \"active_operations\": " << metrics.activeOperations << ",\n"
              << "    \"ready_notices\": " << metrics.readyNotices << ",\n"
              << "    \"owned_bytes\": " << metrics.ownedBytes << "\n"
              << "  }\n"
              << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseConfig(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "io_uring one-shot benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
