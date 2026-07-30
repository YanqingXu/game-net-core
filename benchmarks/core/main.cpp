#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <psapi.h>
#else
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::size_t kCloseReasonCount = 10;

using CloseReasonCounts =
    std::array<std::uint64_t, kCloseReasonCount>;

std::size_t closeReasonIndex(
    gamenet::net::TcpConnectionCloseReason reason) noexcept {
    switch (reason) {
    case gamenet::net::TcpConnectionCloseReason::PeerEof:
        return 0;
    case gamenet::net::TcpConnectionCloseReason::Reset:
        return 1;
    case gamenet::net::TcpConnectionCloseReason::ConnectTimeout:
        return 2;
    case gamenet::net::TcpConnectionCloseReason::InputLimit:
        return 3;
    case gamenet::net::TcpConnectionCloseReason::OutputOverload:
        return 4;
    case gamenet::net::TcpConnectionCloseReason::AdmissionPolicy:
        return 5;
    case gamenet::net::TcpConnectionCloseReason::GracefulShutdown:
        return 6;
    case gamenet::net::TcpConnectionCloseReason::ForcedShutdown:
        return 7;
    case gamenet::net::TcpConnectionCloseReason::CallbackFailure:
        return 8;
    case gamenet::net::TcpConnectionCloseReason::InternalError:
        return 9;
    }
    return 9;
}

struct Config {
    std::string scenario{"echo"};
    std::size_t connections{0};
    std::size_t connectConcurrency{1};
    std::size_t iocpAcceptDepth{4};
    bool preloadBeforeLoop{false};
    std::size_t eventLoopThreads{1};
    std::size_t messagesPerConnection{1000};
    std::size_t payloadBytes{256};
    std::size_t churnTargetPerSecond{1000};
    std::chrono::milliseconds churnDuration{5000};
    std::chrono::milliseconds churnConnectTimeout{1000};
    std::size_t slowBytes{8U * kMebibyte};
    std::size_t highWaterBytes{64U * 1024U};
    std::chrono::milliseconds settle{500};
    std::chrono::milliseconds timeout{30000};
    bool connectionsProvided{false};
};

struct Result {
    double elapsedSeconds{0.0};
    std::uint64_t roundTrips{0};
    std::uint64_t applicationBytes{0};
    std::uint64_t requestedBytes{0};
    std::uint64_t acceptedBytes{0};
    std::uint64_t rejectedBytes{0};
    std::uint64_t acceptedSends{0};
    std::uint64_t rejectedSends{0};
    std::uint64_t overloadedSends{0};
    std::uint64_t closedSends{0};
    std::uint64_t ownerUnavailableSends{0};
    std::uint64_t pendingOutputPeakBytes{0};
    std::uint64_t readPauseObservations{0};
    std::uint64_t readResumeObservations{0};
    std::uint64_t outputLowWaterBytes{
        gamenet::net::TcpConnectionBackpressureOptions{}.lowWaterMarkBytes};
    std::uint64_t outputHighWaterBytes{
        gamenet::net::TcpConnectionBackpressureOptions{}.highWaterMarkBytes};
    std::uint64_t outputHardLimitBytes{
        gamenet::net::TcpConnectionBackpressureOptions{}.hardLimitBytes};
    std::uint64_t maxInputBufferBytes{
        gamenet::net::TcpConnectionBackpressureOptions{}.maxInputBufferBytes};
    std::uint64_t workingSetBefore{0};
    std::uint64_t workingSetAfter{0};
    std::int64_t workingSetDelta{0};
    std::optional<double> throughputMiBPerSecond;
    std::optional<double> messagesPerSecond;
    std::optional<double> p50LatencyUs;
    std::optional<double> p99LatencyUs;
    std::optional<double> p999LatencyUs;
    std::optional<double> approxBytesPerConnection;
    std::optional<double> connectionEstablishSeconds;
    std::optional<double> connectionEstablishPerSecond;
    std::optional<double> idleObservationSeconds;
    std::optional<double> idleProcessCpuSeconds;
    std::optional<double> idleProcessCpuPercent;
    std::optional<double> connectionCloseSeconds;
    std::optional<double> serverStopSeconds;
    std::optional<std::string> serverStopOutcome;
    std::optional<std::uint64_t> serverStopInitialConnections;
    std::optional<std::uint64_t> serverStopForcedConnections;
    std::uint64_t churnAttemptedConnections{0};
    std::uint64_t churnAcceptedConnections{0};
    std::uint64_t churnConnectFailures{0};
    std::uint64_t churnClosedConnections{0};
    std::uint64_t churnBatches{0};
    std::optional<double> churnElapsedSeconds;
    std::optional<double> churnAttemptsPerSecond;
    std::optional<double> churnAcceptsPerSecond;
    std::optional<double> churnClosesPerSecond;
    std::vector<std::uint64_t> churnWorkerAcceptCounts;
    std::optional<double> churnWorkerSkewRatio;
    std::optional<double> churnConnectP99Us;
    std::optional<double> churnConnectMaxUs;
    std::optional<double> churnAcceptP99Us;
    std::optional<double> churnAcceptMaxUs;
    std::optional<double> churnCloseP99Us;
    std::optional<double> churnCloseMaxUs;
    std::optional<double> churnScheduleLagP99Us;
    std::optional<double> churnScheduleLagMaxUs;
    CloseReasonCounts churnCloseReasonCounts{};
    std::optional<double> backpressureRecoverySeconds;
    std::uint64_t highWaterCallbacks{0};
};

class SharedState {
public:
    // condition-variable-predicate-lock: every counter observed by a cv_
    // predicate is published while holding mutex_ before notification.
    void markConnected(const gamenet::net::EventLoop* ownerLoop) {
        {
            std::lock_guard lock(mutex_);
            auto entry = std::find_if(
                workerAcceptCounts_.begin(),
                workerAcceptCounts_.end(),
                [ownerLoop](const auto& candidate) {
                    return candidate.first == ownerLoop;
                });
            if (entry == workerAcceptCounts_.end()) {
                workerAcceptCounts_.emplace_back(ownerLoop, 1);
            } else {
                ++entry->second;
            }
            connected_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void markDisconnected() {
        {
            std::lock_guard lock(mutex_);
            disconnected_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void markClientsCreated() {
        {
            std::lock_guard lock(mutex_);
            clientsCreated_ = true;
        }
        cv_.notify_all();
    }

    bool waitForClientsCreated(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return clientsCreated_ || !failure_.empty();
        }) && clientsCreated_;
    }

    void beginAcceptDrain() {
        std::lock_guard lock(mutex_);
        acceptDrainStarted_ = Clock::now();
    }

    double acceptDrainElapsedSeconds() const {
        std::lock_guard lock(mutex_);
        if (!acceptDrainStarted_) {
            throw std::logic_error("accept drain start was not published");
        }
        return std::chrono::duration<double>(
                   Clock::now() - *acceptDrainStarted_)
            .count();
    }

    void beginClientClose() {
        std::lock_guard lock(mutex_);
        if (!clientCloseStarted_) {
            clientCloseStarted_ = Clock::now();
        }
    }

    std::optional<double> clientCloseElapsedSeconds() const {
        std::lock_guard lock(mutex_);
        if (!clientCloseStarted_) {
            return std::nullopt;
        }
        return std::chrono::duration<double>(
                   Clock::now() - *clientCloseStarted_)
            .count();
    }

    void markHighWater() {
        highWaterCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    void markSlowSend(
        gamenet::net::TcpSendResult result,
        std::size_t requestedBytes,
        std::size_t pendingOutputBytes,
        bool readingPaused) {
        requestedBytes_.fetch_add(requestedBytes, std::memory_order_relaxed);
        switch (result) {
        case gamenet::net::TcpSendResult::Accepted:
            acceptedBytes_.fetch_add(requestedBytes, std::memory_order_relaxed);
            acceptedSends_.fetch_add(1, std::memory_order_relaxed);
            break;
        case gamenet::net::TcpSendResult::Overloaded:
        case gamenet::net::TcpSendResult::LoopOverloaded:
        case gamenet::net::TcpSendResult::ServerOverloaded:
        case gamenet::net::TcpSendResult::GlobalOverloaded:
            rejectedBytes_.fetch_add(requestedBytes, std::memory_order_relaxed);
            rejectedSends_.fetch_add(1, std::memory_order_relaxed);
            overloadedSends_.fetch_add(1, std::memory_order_relaxed);
            break;
        case gamenet::net::TcpSendResult::Closed:
            rejectedBytes_.fetch_add(requestedBytes, std::memory_order_relaxed);
            rejectedSends_.fetch_add(1, std::memory_order_relaxed);
            closedSends_.fetch_add(1, std::memory_order_relaxed);
            break;
        case gamenet::net::TcpSendResult::OwnerUnavailable:
            rejectedBytes_.fetch_add(requestedBytes, std::memory_order_relaxed);
            rejectedSends_.fetch_add(1, std::memory_order_relaxed);
            ownerUnavailableSends_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        auto peak = pendingOutputPeakBytes_.load(std::memory_order_relaxed);
        while (pendingOutputBytes > peak &&
               !pendingOutputPeakBytes_.compare_exchange_weak(
                   peak,
                   pendingOutputBytes,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        if (readingPaused) {
            readPauseObservations_.fetch_add(1, std::memory_order_relaxed);
        }
        slowSendAttempts_.fetch_add(1, std::memory_order_release);
        cv_.notify_all();
    }

    void markSlowWriteComplete(bool readingResumed) {
        if (readingResumed) {
            readResumeObservations_.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard lock(mutex_);
            slowWriteCompletes_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_all();
    }

    bool waitForSlowWriteCompletes(
        std::size_t expected,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return slowWriteCompletes_.load(std::memory_order_acquire) >= expected ||
                   !failure_.empty();
        }) && slowWriteCompletes_.load(std::memory_order_acquire) >= expected;
    }

    bool waitForConnections(std::size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return connected_.load(std::memory_order_acquire) >= expected || !failure_.empty();
        }) && connected_.load(std::memory_order_acquire) >= expected;
    }

    bool waitForDisconnections(
        std::size_t expected,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return disconnected_.load(std::memory_order_acquire) >= expected ||
                   !failure_.empty();
        }) && disconnected_.load(std::memory_order_acquire) >= expected;
    }

    void fail(std::string message) {
        {
            std::lock_guard lock(mutex_);
            if (failure_.empty()) {
                failure_ = std::move(message);
            }
        }
        cv_.notify_all();
    }

    std::string failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    std::size_t connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    std::size_t disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }

    std::vector<std::uint64_t> workerAcceptCounts() const {
        std::lock_guard lock(mutex_);
        std::vector<std::uint64_t> counts;
        counts.reserve(workerAcceptCounts_.size());
        for (const auto& [ownerLoop, count] : workerAcceptCounts_) {
            (void)ownerLoop;
            counts.push_back(count);
        }
        std::sort(counts.begin(), counts.end());
        return counts;
    }

    void markCloseReason(gamenet::net::TcpConnectionCloseReason reason) {
        std::lock_guard lock(mutex_);
        ++closeReasonCounts_[closeReasonIndex(reason)];
    }

    CloseReasonCounts closeReasonCounts() const {
        std::lock_guard lock(mutex_);
        return closeReasonCounts_;
    }

    std::uint64_t highWaterCallbacks() const noexcept {
        return highWaterCallbacks_.load(std::memory_order_relaxed);
    }

    std::uint64_t requestedBytes() const noexcept {
        return requestedBytes_.load(std::memory_order_relaxed);
    }

    std::uint64_t acceptedBytes() const noexcept {
        return acceptedBytes_.load(std::memory_order_relaxed);
    }

    std::uint64_t rejectedBytes() const noexcept {
        return rejectedBytes_.load(std::memory_order_relaxed);
    }

    std::uint64_t acceptedSends() const noexcept {
        return acceptedSends_.load(std::memory_order_relaxed);
    }

    std::uint64_t rejectedSends() const noexcept {
        return rejectedSends_.load(std::memory_order_relaxed);
    }

    std::uint64_t overloadedSends() const noexcept {
        return overloadedSends_.load(std::memory_order_relaxed);
    }

    std::uint64_t closedSends() const noexcept {
        return closedSends_.load(std::memory_order_relaxed);
    }

    std::uint64_t ownerUnavailableSends() const noexcept {
        return ownerUnavailableSends_.load(std::memory_order_relaxed);
    }

    std::uint64_t pendingOutputPeakBytes() const noexcept {
        return pendingOutputPeakBytes_.load(std::memory_order_relaxed);
    }

    std::uint64_t readPauseObservations() const noexcept {
        return readPauseObservations_.load(std::memory_order_relaxed);
    }

    std::uint64_t readResumeObservations() const noexcept {
        return readResumeObservations_.load(std::memory_order_relaxed);
    }

    std::size_t slowSendAttempts() const noexcept {
        return slowSendAttempts_.load(std::memory_order_acquire);
    }

    void markDriverDone() noexcept {
        driverDone_.store(true, std::memory_order_release);
    }

    bool driverDone() const noexcept {
        return driverDone_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::size_t> connected_{0};
    std::atomic<std::size_t> disconnected_{0};
    std::atomic<std::uint64_t> highWaterCallbacks_{0};
    std::atomic<std::uint64_t> requestedBytes_{0};
    std::atomic<std::uint64_t> acceptedBytes_{0};
    std::atomic<std::uint64_t> rejectedBytes_{0};
    std::atomic<std::uint64_t> acceptedSends_{0};
    std::atomic<std::uint64_t> rejectedSends_{0};
    std::atomic<std::uint64_t> overloadedSends_{0};
    std::atomic<std::uint64_t> closedSends_{0};
    std::atomic<std::uint64_t> ownerUnavailableSends_{0};
    std::atomic<std::uint64_t> pendingOutputPeakBytes_{0};
    std::atomic<std::uint64_t> readPauseObservations_{0};
    std::atomic<std::uint64_t> readResumeObservations_{0};
    std::atomic<std::size_t> slowSendAttempts_{0};
    std::atomic<std::size_t> slowWriteCompletes_{0};
    std::atomic<bool> driverDone_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool clientsCreated_{false};
    std::optional<Clock::time_point> acceptDrainStarted_;
    std::optional<Clock::time_point> clientCloseStarted_;
    std::vector<std::pair<const gamenet::net::EventLoop*, std::uint64_t>>
        workerAcceptCounts_;
    CloseReasonCounts closeReasonCounts_{};
    std::string failure_;
};

[[noreturn]] void usageError(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

std::uint64_t parseUnsigned(std::string_view text, std::string_view option) {
    std::uint64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end) {
        usageError(std::string(option) + " requires an unsigned integer");
    }
    return value;
}

std::size_t parseSize(
    std::string_view text,
    std::string_view option,
    std::size_t minimum,
    std::size_t maximum) {
    const auto value = parseUnsigned(text, option);
    if (value < minimum || value > maximum) {
        usageError(std::string(option) + " is outside the supported range");
    }
    return static_cast<std::size_t>(value);
}

void printUsage(std::ostream& output) {
    output
        << "usage: gamenet_core_benchmark [options]\n"
        << "  --scenario echo|connections|connection-churn|slow-client\n"
        << "  --connections N       scenario default: echo=4, connections=256, churn=100, slow-client=4\n"
        << "  --connect-concurrency N simultaneous client connect workers (default 1)\n"
        << "  --iocp-accept-depth N Windows AcceptEx pre-post depth (default 4)\n"
        << "  --preload-before-loop 0|1 preload connections before base loop (default 0)\n"
        << "  --threads N           TcpServer worker EventLoops, including 0 for the base loop\n"
        << "  --messages N          echo messages per connection (default 1000)\n"
        << "  --payload N           echo payload bytes (default 256)\n"
        << "  --churn-rate N        target connection attempts per second (default 1000)\n"
        << "  --churn-duration-ms N paced churn duration (default 5000)\n"
        << "  --churn-connect-timeout-ms N per-attempt deadline (default 1000)\n"
        << "  --slow-bytes N        bytes offered to each slow client (default 8388608)\n"
        << "  --high-water N        output pause/callback threshold (default 65536)\n"
        << "  --settle-ms N         settle interval before memory sampling (default 500)\n"
        << "  --timeout-ms N        connection/I/O and overall timeout (default 30000)\n";
}

Config parseArgs(int argc, char* argv[]) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            printUsage(std::cerr);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            usageError(std::string(option) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (option == "--scenario") {
            config.scenario = value;
        } else if (option == "--connections") {
            config.connections = parseSize(value, option, 1, 100000);
            config.connectionsProvided = true;
        } else if (option == "--connect-concurrency") {
            config.connectConcurrency = parseSize(value, option, 1, 1024);
        } else if (option == "--iocp-accept-depth") {
            config.iocpAcceptDepth = parseSize(value, option, 1, 64);
        } else if (option == "--preload-before-loop") {
            config.preloadBeforeLoop =
                parseSize(value, option, 0, 1) != 0;
        } else if (option == "--threads") {
            config.eventLoopThreads = parseSize(value, option, 0, 64);
        } else if (option == "--messages") {
            config.messagesPerConnection = parseSize(value, option, 1, 1000000);
        } else if (option == "--payload") {
            config.payloadBytes = parseSize(value, option, 1, 64U * kMebibyte);
        } else if (option == "--churn-rate") {
            config.churnTargetPerSecond =
                parseSize(value, option, 1, 100000);
        } else if (option == "--churn-duration-ms") {
            config.churnDuration =
                std::chrono::milliseconds(parseSize(value, option, 100, 600000));
        } else if (option == "--churn-connect-timeout-ms") {
            config.churnConnectTimeout =
                std::chrono::milliseconds(parseSize(value, option, 1, 30000));
        } else if (option == "--slow-bytes") {
            config.slowBytes = parseSize(value, option, 1, 256U * kMebibyte);
        } else if (option == "--high-water") {
            config.highWaterBytes = parseSize(
                value,
                option,
                2,
                gamenet::net::TcpConnectionBackpressureOptions{}.hardLimitBytes - 1);
        } else if (option == "--settle-ms") {
            config.settle = std::chrono::milliseconds(parseSize(value, option, 0, 60000));
        } else if (option == "--timeout-ms") {
            config.timeout = std::chrono::milliseconds(parseSize(value, option, 100, 300000));
        } else {
            usageError(std::string("unknown option: ") + std::string(option));
        }
    }

    if (config.scenario != "echo" && config.scenario != "connections" &&
        config.scenario != "connection-churn" &&
        config.scenario != "slow-client") {
        usageError(
            "--scenario must be echo, connections, connection-churn, or "
            "slow-client");
    }
    if (!config.connectionsProvided) {
        config.connections =
            config.scenario == "connections"
            ? 256U
            : (config.scenario == "connection-churn" ? 100U : 4U);
    }
    if (config.connectConcurrency > config.connections) {
        usageError("--connect-concurrency must not exceed --connections");
    }
    if (config.preloadBeforeLoop && config.scenario != "connections") {
        usageError("--preload-before-loop is supported only for connections");
    }
    if (config.highWaterBytes > config.slowBytes && config.scenario == "slow-client") {
        usageError("--high-water must not exceed --slow-bytes for slow-client");
    }
    if (config.scenario == "connection-churn") {
        const auto attempts =
            static_cast<std::uint64_t>(config.churnTargetPerSecond) *
            static_cast<std::uint64_t>(config.churnDuration.count()) /
            1000U;
        if (attempts == 0) {
            usageError(
                "--churn-rate and --churn-duration-ms produce zero attempts");
        }
        if (config.churnDuration > config.timeout) {
            usageError(
                "--churn-duration-ms must not exceed --timeout-ms");
        }
        if (config.churnConnectTimeout > config.timeout) {
            usageError(
                "--churn-connect-timeout-ms must not exceed --timeout-ms");
        }
    }
    return config;
}

std::uint64_t sampleWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) == FALSE) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        throw std::runtime_error("failed to read /proc/self/statm");
    }
    (void)totalPages;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");
    }
    return residentPages * static_cast<std::uint64_t>(pageSize);
#endif
}

double sampleProcessCpuSeconds() {
#ifdef _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(
            ::GetCurrentProcess(),
            &created,
            &exited,
            &kernel,
            &user) == FALSE) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    ULARGE_INTEGER kernelTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTicks{};
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    constexpr double kFiletimeTicksPerSecond = 10000000.0;
    return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) /
           kFiletimeTicksPerSecond;
#else
    timespec processTime{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &processTime) != 0) {
        const int error = gamenet::net::sockets::lastError();
        throw std::runtime_error(
            "clock_gettime(CLOCK_PROCESS_CPUTIME_ID): " +
            gamenet::net::sockets::errorMessage(error));
    }
    return static_cast<double>(processTime.tv_sec) +
           static_cast<double>(processTime.tv_nsec) / 1000000000.0;
#endif
}

std::string socketFailure(std::string_view operation) {
    const int error = gamenet::net::sockets::lastError();
    return std::string(operation) + ": " + gamenet::net::sockets::errorMessage(error);
}

class ClientSocket {
public:
    ClientSocket() = default;
    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    ClientSocket(ClientSocket&& other) noexcept : fd_(std::exchange(other.fd_, gamenet::net::kInvalidSocket)) {}

    ClientSocket& operator=(ClientSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, gamenet::net::kInvalidSocket);
        }
        return *this;
    }

    ~ClientSocket() {
        close();
    }

    bool valid() const noexcept {
        return gamenet::net::sockets::isValid(fd_);
    }

    static ClientSocket connectTo(
        const gamenet::net::InetAddress& address,
        std::chrono::milliseconds timeout,
        bool slowReader) {
        gamenet::net::sockets::ensureInitialized();
        ClientSocket socket;
        socket.fd_ = ::socket(address.family(), SOCK_STREAM, IPPROTO_TCP);
        if (!gamenet::net::sockets::isValid(socket.fd_)) {
            throw std::runtime_error(socketFailure("socket"));
        }
        socket.configure(timeout, slowReader);
        if (::connect(socket.fd_, address.getSockAddr(), address.getSockAddrLen()) != 0) {
            throw std::runtime_error(socketFailure("connect"));
        }
        return socket;
    }

    static ClientSocket connectToWithDeadline(
        const gamenet::net::InetAddress& address,
        std::chrono::milliseconds timeout) {
        gamenet::net::sockets::ensureInitialized();
        ClientSocket socket;
        socket.fd_ =
            gamenet::net::sockets::createNonblocking(address.family());
        if (!gamenet::net::sockets::isValid(socket.fd_)) {
            throw std::runtime_error(socketFailure("socket"));
        }
        socket.configure(timeout, false);
        if (::connect(
                socket.fd_,
                address.getSockAddr(),
                address.getSockAddrLen()) != 0) {
            const int connectError = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isInProgress(connectError) &&
                !gamenet::net::sockets::isWouldBlock(connectError)) {
                throw std::runtime_error(
                    "connect: " +
                    gamenet::net::sockets::errorMessage(connectError));
            }
            socket.waitForConnect(timeout);
        }
        socket.setBlocking();
        return socket;
    }

    void sendAll(std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto remaining = data.size() - sent;
            const int count = static_cast<int>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<int>::max)())));
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto written = ::send(fd_, data.data() + sent, count, flags);
            if (written <= 0) {
                throw std::runtime_error(socketFailure("send"));
            }
            sent += static_cast<std::size_t>(written);
        }
    }

    void receiveExact(std::string& destination) {
        std::size_t received = 0;
        while (received < destination.size()) {
            const auto remaining = destination.size() - received;
            const int count = static_cast<int>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const auto read = ::recv(fd_, destination.data() + received, count, 0);
            if (read <= 0) {
                throw std::runtime_error(socketFailure("recv"));
            }
            received += static_cast<std::size_t>(read);
        }
    }

    void closeAbortively() noexcept {
        if (!gamenet::net::sockets::isValid(fd_)) {
            return;
        }
        linger resetOnClose{};
        resetOnClose.l_onoff = 1;
        resetOnClose.l_linger = 0;
        (void)::setsockopt(
            fd_,
            SOL_SOCKET,
            SO_LINGER,
            reinterpret_cast<const char*>(&resetOnClose),
            static_cast<socklen_t>(sizeof(resetOnClose)));
        close();
    }

private:
    void waitForConnect(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (true) {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                throw std::runtime_error("connect: deadline expired");
            }
            const auto remainingMicroseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    remaining);
            timeval timeoutValue{};
            timeoutValue.tv_sec = static_cast<decltype(timeoutValue.tv_sec)>(
                remainingMicroseconds.count() / 1000000);
            timeoutValue.tv_usec =
                static_cast<decltype(timeoutValue.tv_usec)>(
                    remainingMicroseconds.count() % 1000000);
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(fd_, &writable);
            FD_SET(fd_, &failed);
#ifdef _WIN32
            constexpr int descriptorCount = 0;
#else
            const int descriptorCount = fd_ + 1;
#endif
            const int ready = ::select(
                descriptorCount,
                nullptr,
                &writable,
                &failed,
                &timeoutValue);
            if (ready > 0) {
                const int socketError =
                    gamenet::net::sockets::getSocketError(fd_);
                if (socketError != 0) {
                    throw std::runtime_error(
                        "connect: " +
                        gamenet::net::sockets::errorMessage(socketError));
                }
                return;
            }
            if (ready == 0) {
                throw std::runtime_error("connect: deadline expired");
            }
            const int selectError = gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isInterrupted(selectError)) {
                throw std::runtime_error(
                    "select(connect): " +
                    gamenet::net::sockets::errorMessage(selectError));
            }
        }
    }

    void setBlocking() {
#ifdef _WIN32
        u_long nonBlocking = 0;
        if (::ioctlsocket(fd_, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            throw std::runtime_error(socketFailure("ioctlsocket(FIONBIO=0)"));
        }
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK) < 0) {
            throw std::runtime_error(socketFailure("fcntl(O_NONBLOCK=0)"));
        }
#endif
    }

    void configure(std::chrono::milliseconds timeout, bool slowReader) {
        const int noDelay = 1;
        if (::setsockopt(
                fd_,
                IPPROTO_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay),
                static_cast<socklen_t>(sizeof(noDelay))) != 0) {
            throw std::runtime_error(socketFailure("setsockopt(TCP_NODELAY)"));
        }

#ifdef _WIN32
        const DWORD timeoutValue = static_cast<DWORD>(timeout.count());
        if (::setsockopt(
                fd_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutValue),
                static_cast<socklen_t>(sizeof(timeoutValue))) != 0 ||
            ::setsockopt(
                fd_,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeoutValue),
                static_cast<socklen_t>(sizeof(timeoutValue))) != 0) {
            throw std::runtime_error(socketFailure("setsockopt(socket timeout)"));
        }
#else
        timeval timeoutValue{};
        timeoutValue.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        timeoutValue.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeoutValue, sizeof(timeoutValue)) != 0 ||
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeoutValue, sizeof(timeoutValue)) != 0) {
            throw std::runtime_error(socketFailure("setsockopt(socket timeout)"));
        }
#endif

        if (slowReader) {
            const int receiveBufferBytes = 4096;
            if (::setsockopt(
                    fd_,
                    SOL_SOCKET,
                    SO_RCVBUF,
                    reinterpret_cast<const char*>(&receiveBufferBytes),
                    static_cast<socklen_t>(sizeof(receiveBufferBytes))) != 0) {
                throw std::runtime_error(socketFailure("setsockopt(SO_RCVBUF)"));
            }
        }
    }

    void close() noexcept {
        if (gamenet::net::sockets::isValid(fd_)) {
            gamenet::net::sockets::close(fd_);
            fd_ = gamenet::net::kInvalidSocket;
        }
    }

    gamenet::net::SocketFd fd_{gamenet::net::kInvalidSocket};
};

std::vector<ClientSocket> connectClients(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    bool slowReaders) {
    std::vector<ClientSocket> clients(config.connections);
    if (config.connectConcurrency == 1) {
        for (std::size_t index = 0; index < config.connections; ++index) {
            clients[index] =
                ClientSocket::connectTo(address, config.timeout, slowReaders);
        }
    } else {
        const std::size_t workerCount =
            (std::min)(config.connectConcurrency, config.connections);
        std::atomic<std::size_t> nextIndex{0};
        std::atomic<bool> failed{false};
        std::mutex failureMutex;
        std::exception_ptr failure;
        std::barrier startGate(
            static_cast<std::ptrdiff_t>(workerCount + 1));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&] {
                startGate.arrive_and_wait();
                try {
                    while (!failed.load(std::memory_order_acquire)) {
                        const std::size_t index =
                            nextIndex.fetch_add(1, std::memory_order_relaxed);
                        if (index >= config.connections) {
                            break;
                        }
                        clients[index] = ClientSocket::connectTo(
                            address,
                            config.timeout,
                            slowReaders);
                    }
                } catch (...) {
                    {
                        std::lock_guard lock(failureMutex);
                        if (!failure) {
                            failure = std::current_exception();
                        }
                    }
                    failed.store(true, std::memory_order_release);
                }
            });
        }
        startGate.arrive_and_wait();
        for (auto& worker : workers) {
            worker.join();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    state.markClientsCreated();
    if (!state.waitForConnections(config.connections, config.timeout)) {
        throw std::runtime_error("timed out waiting for TcpServer connection callbacks");
    }
    return clients;
}

struct ChurnClientBatch {
    std::vector<ClientSocket> clients;
    std::size_t connectFailures{0};
};

class ChurnConnectorPool {
public:
    ChurnConnectorPool(
        const Config& config,
        gamenet::net::InetAddress address)
        : address_(std::move(address)),
          timeout_(config.churnConnectTimeout),
          workerCount_((std::min)(
              config.connectConcurrency,
              config.connections)) {
        workers_.reserve(workerCount_);
        for (std::size_t worker = 0; worker < workerCount_; ++worker) {
            workers_.emplace_back([this] { workerMain(); });
        }
    }

    ChurnConnectorPool(const ChurnConnectorPool&) = delete;
    ChurnConnectorPool& operator=(const ChurnConnectorPool&) = delete;

    ~ChurnConnectorPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    ChurnClientBatch connectBatch(std::size_t attemptCount) {
        {
            std::lock_guard lock(mutex_);
            slots_.clear();
            slots_.resize(attemptCount);
            attemptCount_ = attemptCount;
            nextIndex_.store(0, std::memory_order_relaxed);
            connectFailures_.store(0, std::memory_order_relaxed);
            completedWorkers_ = 0;
            ++generation_;
        }
        workAvailable_.notify_all();

        {
            std::unique_lock lock(mutex_);
            batchComplete_.wait(lock, [this] {
                return completedWorkers_ == workerCount_;
            });
        }

        ChurnClientBatch batch;
        batch.clients.reserve(
            attemptCount -
            connectFailures_.load(std::memory_order_relaxed));
        for (auto& slot : slots_) {
            if (slot.valid()) {
                batch.clients.push_back(std::move(slot));
            }
        }
        batch.connectFailures =
            connectFailures_.load(std::memory_order_relaxed);
        return batch;
    }

private:
    void workerMain() {
        std::uint64_t observedGeneration = 0;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                workAvailable_.wait(lock, [&] {
                    return stopping_ || generation_ != observedGeneration;
                });
                if (stopping_) {
                    return;
                }
                observedGeneration = generation_;
            }

            while (true) {
                const std::size_t index =
                    nextIndex_.fetch_add(1, std::memory_order_relaxed);
                if (index >= attemptCount_) {
                    break;
                }
                try {
                    slots_[index] =
                        ClientSocket::connectToWithDeadline(address_, timeout_);
                } catch (const std::exception&) {
                    connectFailures_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }

            {
                std::lock_guard lock(mutex_);
                ++completedWorkers_;
                if (completedWorkers_ == workerCount_) {
                    batchComplete_.notify_one();
                }
            }
        }
    }

    gamenet::net::InetAddress address_;
    std::chrono::milliseconds timeout_;
    std::size_t workerCount_;
    std::vector<ClientSocket> slots_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> nextIndex_{0};
    std::atomic<std::size_t> connectFailures_{0};
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable batchComplete_;
    std::size_t attemptCount_{0};
    std::size_t completedWorkers_{0};
    std::uint64_t generation_{0};
    bool stopping_{false};
};

void recordWorkingSet(Result& result, const Config& config) {
    result.workingSetAfter = sampleWorkingSetBytes();
    if (result.workingSetAfter >= result.workingSetBefore) {
        result.workingSetDelta = static_cast<std::int64_t>(result.workingSetAfter - result.workingSetBefore);
    } else {
        result.workingSetDelta = -static_cast<std::int64_t>(result.workingSetBefore - result.workingSetAfter);
    }
    result.approxBytesPerConnection =
        static_cast<double>(result.workingSetDelta) / static_cast<double>(config.connections);
}

void observeIdleProcessCpu(const Config& config, Result& result) {
    const auto wallStarted = Clock::now();
    const double cpuStarted = sampleProcessCpuSeconds();
    std::this_thread::sleep_for(config.settle);
    const double cpuFinished = sampleProcessCpuSeconds();
    const auto wallFinished = Clock::now();
    if (cpuFinished < cpuStarted) {
        throw std::runtime_error("process CPU clock moved backwards");
    }
    const double wallSeconds =
        std::chrono::duration<double>(wallFinished - wallStarted).count();
    if (wallSeconds <= 0.0) {
        throw std::runtime_error("idle observation clock did not advance");
    }
    const double cpuSeconds = cpuFinished - cpuStarted;
    result.idleObservationSeconds = wallSeconds;
    result.idleProcessCpuSeconds = cpuSeconds;
    result.idleProcessCpuPercent = cpuSeconds * 100.0 / wallSeconds;
}

double nearestRankPercentile(
    const std::vector<double>& sortedSamples,
    double fraction) {
    if (sortedSamples.empty()) {
        throw std::runtime_error("cannot calculate percentile without samples");
    }
    const double rank =
        std::ceil(fraction * static_cast<double>(sortedSamples.size()));
    const auto nearestRank = static_cast<std::size_t>(rank < 1.0 ? 1.0 : rank);
    return sortedSamples[
        (std::min)(nearestRank - 1, sortedSamples.size() - 1)];
}

void runEcho(
    const Config& config,
    const gamenet::net::InetAddress& address,
    const std::string& payload,
    SharedState& state,
    Result& result) {
    auto clients = connectClients(config, address, state, false);
    std::this_thread::sleep_for(config.settle);
    recordWorkingSet(result, config);

    std::barrier startGate(static_cast<std::ptrdiff_t>(config.connections + 1));
    std::vector<std::vector<double>> samples(config.connections);
    std::vector<std::thread> workers;
    workers.reserve(config.connections);
    for (std::size_t clientIndex = 0; clientIndex < config.connections; ++clientIndex) {
        workers.emplace_back([&, clientIndex] {
            auto& localSamples = samples[clientIndex];
            localSamples.reserve(config.messagesPerConnection);
            std::string response(config.payloadBytes, '\0');
            startGate.arrive_and_wait();
            try {
                for (std::size_t message = 0; message < config.messagesPerConnection; ++message) {
                    const auto started = Clock::now();
                    clients[clientIndex].sendAll(payload);
                    clients[clientIndex].receiveExact(response);
                    const auto finished = Clock::now();
                    if (response != payload) {
                        throw std::runtime_error("echo payload mismatch");
                    }
                    localSamples.push_back(
                        std::chrono::duration<double, std::micro>(finished - started).count());
                }
            } catch (const std::exception& error) {
                state.fail(error.what());
            }
        });
    }

    const auto started = Clock::now();
    startGate.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto finished = Clock::now();

    std::vector<double> allSamples;
    for (const auto& local : samples) {
        allSamples.insert(allSamples.end(), local.begin(), local.end());
    }
    result.elapsedSeconds = std::chrono::duration<double>(finished - started).count();
    result.roundTrips = static_cast<std::uint64_t>(allSamples.size());
    result.applicationBytes = result.roundTrips * static_cast<std::uint64_t>(config.payloadBytes) * 2U;
    if (!allSamples.empty() && result.elapsedSeconds > 0.0) {
        result.throughputMiBPerSecond =
            static_cast<double>(result.applicationBytes) /
            static_cast<double>(kMebibyte) /
            result.elapsedSeconds;
        result.messagesPerSecond =
            static_cast<double>(result.roundTrips) / result.elapsedSeconds;
        std::sort(allSamples.begin(), allSamples.end());
        result.p50LatencyUs = nearestRankPercentile(allSamples, 0.50);
        result.p99LatencyUs = nearestRankPercentile(allSamples, 0.99);
        result.p999LatencyUs = nearestRankPercentile(allSamples, 0.999);
    }
    state.beginClientClose();
    clients.clear();
}

void runConnections(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const auto started = Clock::now();
    auto clients = connectClients(config, address, state, false);
    const double establishSeconds =
        config.preloadBeforeLoop
        ? state.acceptDrainElapsedSeconds()
        : std::chrono::duration<double>(Clock::now() - started).count();
    if (establishSeconds <= 0.0) {
        throw std::runtime_error("connection establishment clock did not advance");
    }
    result.connectionEstablishSeconds = establishSeconds;
    result.connectionEstablishPerSecond =
        static_cast<double>(config.connections) / establishSeconds;
    observeIdleProcessCpu(config, result);
    recordWorkingSet(result, config);
    result.elapsedSeconds =
        config.preloadBeforeLoop
        ? state.acceptDrainElapsedSeconds()
        : std::chrono::duration<double>(Clock::now() - started).count();
    state.beginClientClose();
    for (auto& client : clients) {
        client.closeAbortively();
    }
    clients.clear();
}

void runConnectionChurn(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const std::uint64_t expectedAttempts =
        static_cast<std::uint64_t>(config.churnTargetPerSecond) *
        static_cast<std::uint64_t>(config.churnDuration.count()) /
        1000U;
    std::uint64_t attempted = 0;
    std::uint64_t accepted = 0;
    std::uint64_t connectFailures = 0;
    std::vector<double> connectPhaseUs;
    std::vector<double> acceptPhaseUs;
    std::vector<double> closePhaseUs;
    std::vector<double> scheduleLagUs;
    const auto started = Clock::now();

    {
        ChurnConnectorPool connectorPool(config, address);
        while (attempted < expectedAttempts) {
            const std::size_t batchAttempts = static_cast<std::size_t>(
                (std::min)(
                    static_cast<std::uint64_t>(config.connections),
                    expectedAttempts - attempted));
            const std::uint64_t scheduledAttempts =
                attempted + batchAttempts;
            const auto scheduledAt =
                started +
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(
                        static_cast<double>(scheduledAttempts) /
                        static_cast<double>(config.churnTargetPerSecond)));
            std::this_thread::sleep_until(scheduledAt);

            const auto connectStarted = Clock::now();
            auto batch = connectorPool.connectBatch(batchAttempts);
            const auto connectFinished = Clock::now();
            attempted = scheduledAttempts;
            connectFailures += batch.connectFailures;
            accepted += batch.clients.size();
            if (!state.waitForConnections(
                    static_cast<std::size_t>(accepted),
                    config.timeout)) {
                throw std::runtime_error(
                    "timed out waiting for churn accept callbacks");
            }
            const auto acceptFinished = Clock::now();

            if (attempted == expectedAttempts) {
                state.beginClientClose();
            }
            const auto closeStarted = Clock::now();
            for (auto& client : batch.clients) {
                client.closeAbortively();
            }
            batch.clients.clear();
            if (!state.waitForDisconnections(
                    static_cast<std::size_t>(accepted),
                    config.timeout)) {
                throw std::runtime_error(
                    "timed out waiting for churn disconnect callbacks");
            }
            const auto closeFinished = Clock::now();
            connectPhaseUs.push_back(
                std::chrono::duration<double, std::micro>(
                    connectFinished - connectStarted)
                    .count());
            acceptPhaseUs.push_back(
                std::chrono::duration<double, std::micro>(
                    acceptFinished - connectFinished)
                    .count());
            closePhaseUs.push_back(
                std::chrono::duration<double, std::micro>(
                    closeFinished - closeStarted)
                    .count());
            scheduleLagUs.push_back((std::max)(
                0.0,
                std::chrono::duration<double, std::micro>(
                    closeFinished - scheduledAt)
                    .count()));
        }
    }

    const auto finished = Clock::now();
    const double elapsedSeconds =
        std::chrono::duration<double>(finished - started).count();
    if (elapsedSeconds <= 0.0) {
        throw std::runtime_error("connection churn clock did not advance");
    }
    result.elapsedSeconds = elapsedSeconds;
    result.churnElapsedSeconds = elapsedSeconds;
    result.churnAttemptedConnections = attempted;
    result.churnAcceptedConnections = state.connected();
    result.churnConnectFailures = connectFailures;
    result.churnClosedConnections = state.disconnected();
    result.churnBatches = connectPhaseUs.size();
    result.churnAttemptsPerSecond =
        static_cast<double>(attempted) / elapsedSeconds;
    result.churnAcceptsPerSecond =
        static_cast<double>(result.churnAcceptedConnections) /
        elapsedSeconds;
    result.churnClosesPerSecond =
        static_cast<double>(result.churnClosedConnections) /
        elapsedSeconds;
    result.churnWorkerAcceptCounts = state.workerAcceptCounts();
    const std::size_t expectedWorkerCount =
        (std::max)(std::size_t{1}, config.eventLoopThreads);
    if (result.churnWorkerAcceptCounts.size() > expectedWorkerCount) {
        throw std::runtime_error(
            "churn observed more owner loops than configured");
    }
    result.churnWorkerAcceptCounts.resize(expectedWorkerCount, 0);
    std::sort(
        result.churnWorkerAcceptCounts.begin(),
        result.churnWorkerAcceptCounts.end());
    if (result.churnAcceptedConnections != 0) {
        const double mean =
            static_cast<double>(result.churnAcceptedConnections) /
            static_cast<double>(expectedWorkerCount);
        result.churnWorkerSkewRatio =
            static_cast<double>(
                result.churnWorkerAcceptCounts.back() -
                result.churnWorkerAcceptCounts.front()) /
            mean;
    }
    auto recordPhase = [](std::vector<double>& samples,
                          std::optional<double>& p99,
                          std::optional<double>& maximum) {
        std::sort(samples.begin(), samples.end());
        p99 = nearestRankPercentile(samples, 0.99);
        maximum = samples.back();
    };
    recordPhase(
        connectPhaseUs,
        result.churnConnectP99Us,
        result.churnConnectMaxUs);
    recordPhase(
        acceptPhaseUs,
        result.churnAcceptP99Us,
        result.churnAcceptMaxUs);
    recordPhase(
        closePhaseUs,
        result.churnCloseP99Us,
        result.churnCloseMaxUs);
    recordPhase(
        scheduleLagUs,
        result.churnScheduleLagP99Us,
        result.churnScheduleLagMaxUs);
    recordWorkingSet(result, config);
}

void runSlowClient(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const auto started = Clock::now();
    auto clients = connectClients(config, address, state, true);
    if (state.slowSendAttempts() != config.connections) {
        throw std::runtime_error("slow-client send-attempt accounting did not converge");
    }
    std::this_thread::sleep_for(config.settle);
    recordWorkingSet(result, config);

    const auto acceptedSends = state.acceptedSends();
    if (acceptedSends != 0 && acceptedSends != config.connections) {
        throw std::runtime_error(
            "slow-client mixed accepted/rejected sends cannot map responses to clients");
    }
    if (acceptedSends != 0) {
        const auto recoveryStarted = Clock::now();
        std::vector<std::thread> readers;
        readers.reserve(config.connections);
        for (std::size_t index = 0; index < config.connections; ++index) {
            readers.emplace_back([&, index] {
                try {
                    std::string received(config.slowBytes, '\0');
                    clients[index].receiveExact(received);
                } catch (const std::exception& error) {
                    state.fail(error.what());
                }
            });
        }
        for (auto& reader : readers) {
            reader.join();
        }
        if (!state.failure().empty()) {
            throw std::runtime_error(state.failure());
        }
        if (!state.waitForSlowWriteCompletes(acceptedSends, config.timeout)) {
            throw std::runtime_error(
                "timed out waiting for slow-client output drain and backpressure recovery");
        }
        result.backpressureRecoverySeconds =
            std::chrono::duration<double>(Clock::now() - recoveryStarted).count();
    }

    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    state.beginClientClose();
    clients.clear();
}

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

template <typename Value>
void printOptional(std::ostream& output, const std::optional<Value>& value) {
    if (value) {
        output << *value;
    } else {
        output << "null";
    }
}

void printOptionalString(
    std::ostream& output,
    const std::optional<std::string>& value) {
    if (value) {
        output << '"' << jsonEscape(*value) << '"';
    } else {
        output << "null";
    }
}

void printIntegerArray(
    std::ostream& output,
    const std::vector<std::uint64_t>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << values[index];
    }
    output << ']';
}

void printCloseReasonCounts(
    std::ostream& output,
    const CloseReasonCounts& counts) {
    constexpr std::array<std::string_view, kCloseReasonCount> names{
        "peer_eof",
        "reset",
        "connect_timeout",
        "input_limit",
        "output_overload",
        "admission_policy",
        "graceful_shutdown",
        "forced_shutdown",
        "callback_failure",
        "internal_error",
    };
    output << '{';
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << '"' << names[index] << "\": " << counts[index];
    }
    output << '}';
}

std::string_view platformName() noexcept {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::string_view backendName() noexcept {
#ifdef _WIN32
    return "iocp";
#else
    return "epoll";
#endif
}

std::string_view completionMode() noexcept {
#ifdef _WIN32
    return "get_queued_completion_status_ex_batch_64";
#else
    return "epoll_wait_batch";
#endif
}

std::string_view stopOutcomeName(
    gamenet::net::TcpServerStopOutcome outcome) noexcept {
    switch (outcome) {
    case gamenet::net::TcpServerStopOutcome::Drained:
        return "drained";
    case gamenet::net::TcpServerStopOutcome::ForcedAfterTimeout:
        return "forced_after_timeout";
    case gamenet::net::TcpServerStopOutcome::ForcedByImmediateStop:
        return "forced_by_immediate_stop";
    case gamenet::net::TcpServerStopOutcome::AlreadyStopped:
        return "already_stopped";
    case gamenet::net::TcpServerStopOutcome::ServerDestroyed:
        return "server_destroyed";
    case gamenet::net::TcpServerStopOutcome::SchedulingFailed:
        return "scheduling_failed";
    }
    return "unknown";
}

void printResult(const Config& config, const Result& result, const SharedState& state) {
    const std::string failure = state.failure();
    std::cout << std::fixed << std::setprecision(9)
              << "{\n"
              << "  \"schema\": \"gamenet.core_benchmark.v2\",\n"
              << "  \"status\": \"" << (failure.empty() ? "ok" : "error") << "\",\n"
              << "  \"error\": ";
    if (failure.empty()) {
        std::cout << "null";
    } else {
        std::cout << '"' << jsonEscape(failure) << '"';
    }
    std::cout << ",\n"
              << "  \"scenario\": \"" << config.scenario << "\",\n"
              << "  \"platform\": \"" << platformName() << "\",\n"
              << "  \"backend\": \"" << backendName() << "\",\n"
              << "  \"completion_mode\": \"" << completionMode() << "\",\n"
              << "  \"backpressure_policy\": \"bounded_output_hysteresis\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"parameters\": {\n"
              << "    \"connections\": " << config.connections << ",\n"
              << "    \"connect_concurrency\": " << config.connectConcurrency << ",\n"
              << "    \"iocp_accept_depth\": " << config.iocpAcceptDepth << ",\n"
              << "    \"preload_before_loop\": "
              << (config.preloadBeforeLoop ? "true" : "false") << ",\n"
              << "    \"event_loop_threads\": " << config.eventLoopThreads << ",\n"
              << "    \"messages_per_connection\": " << config.messagesPerConnection << ",\n"
              << "    \"payload_bytes\": " << config.payloadBytes << ",\n"
              << "    \"churn_target_per_second\": "
              << config.churnTargetPerSecond << ",\n"
              << "    \"churn_duration_ms\": "
              << config.churnDuration.count() << ",\n"
              << "    \"churn_connect_timeout_ms\": "
              << config.churnConnectTimeout.count() << ",\n"
              << "    \"slow_bytes_per_connection\": " << config.slowBytes << ",\n"
              << "    \"low_water_bytes\": " << result.outputLowWaterBytes << ",\n"
              << "    \"high_water_bytes\": " << result.outputHighWaterBytes << ",\n"
              << "    \"hard_limit_bytes\": " << result.outputHardLimitBytes << ",\n"
              << "    \"max_input_buffer_bytes\": " << result.maxInputBufferBytes << ",\n"
              << "    \"settle_ms\": " << config.settle.count() << ",\n"
              << "    \"timeout_ms\": " << config.timeout.count() << "\n"
              << "  },\n"
              << "  \"measurements\": {\n"
              << "    \"elapsed_seconds\": " << result.elapsedSeconds << ",\n"
              << "    \"round_trips\": " << result.roundTrips << ",\n"
              << "    \"application_bytes\": " << result.applicationBytes << ",\n"
              << "    \"throughput_mib_per_second\": ";
    printOptional(std::cout, result.throughputMiBPerSecond);
    std::cout << ",\n    \"messages_per_second\": ";
    printOptional(std::cout, result.messagesPerSecond);
    std::cout << ",\n    \"p50_latency_us\": ";
    printOptional(std::cout, result.p50LatencyUs);
    std::cout << ",\n    \"p99_latency_us\": ";
    printOptional(std::cout, result.p99LatencyUs);
    std::cout << ",\n    \"p999_latency_us\": ";
    printOptional(std::cout, result.p999LatencyUs);
    std::cout << ",\n"
              << "    \"working_set_before_bytes\": " << result.workingSetBefore << ",\n"
              << "    \"working_set_after_bytes\": " << result.workingSetAfter << ",\n"
              << "    \"working_set_delta_bytes\": " << result.workingSetDelta << ",\n"
              << "    \"approx_bytes_per_connection\": ";
    printOptional(std::cout, result.approxBytesPerConnection);
    std::cout << ",\n    \"connection_establish_seconds\": ";
    printOptional(std::cout, result.connectionEstablishSeconds);
    std::cout << ",\n    \"connection_establish_per_second\": ";
    printOptional(std::cout, result.connectionEstablishPerSecond);
    std::cout << ",\n    \"idle_observation_seconds\": ";
    printOptional(std::cout, result.idleObservationSeconds);
    std::cout << ",\n    \"idle_process_cpu_seconds\": ";
    printOptional(std::cout, result.idleProcessCpuSeconds);
    std::cout << ",\n    \"idle_process_cpu_percent\": ";
    printOptional(std::cout, result.idleProcessCpuPercent);
    std::cout << ",\n    \"connection_close_seconds\": ";
    printOptional(std::cout, result.connectionCloseSeconds);
    std::cout << ",\n    \"server_stop_seconds\": ";
    printOptional(std::cout, result.serverStopSeconds);
    std::cout << ",\n    \"server_stop_outcome\": ";
    printOptionalString(std::cout, result.serverStopOutcome);
    std::cout << ",\n    \"server_stop_initial_connections\": ";
    printOptional(std::cout, result.serverStopInitialConnections);
    std::cout << ",\n    \"server_stop_forced_connections\": ";
    printOptional(std::cout, result.serverStopForcedConnections);
    std::cout << ",\n"
              << "    \"churn_attempted_connections\": "
              << result.churnAttemptedConnections << ",\n"
              << "    \"churn_accepted_connections\": "
              << result.churnAcceptedConnections << ",\n"
              << "    \"churn_connect_failures\": "
              << result.churnConnectFailures << ",\n"
              << "    \"churn_closed_connections\": "
              << result.churnClosedConnections << ",\n"
              << "    \"churn_batches\": "
              << result.churnBatches;
    std::cout << ",\n    \"churn_elapsed_seconds\": ";
    printOptional(std::cout, result.churnElapsedSeconds);
    std::cout << ",\n    \"churn_attempts_per_second\": ";
    printOptional(std::cout, result.churnAttemptsPerSecond);
    std::cout << ",\n    \"churn_accepts_per_second\": ";
    printOptional(std::cout, result.churnAcceptsPerSecond);
    std::cout << ",\n    \"churn_closes_per_second\": ";
    printOptional(std::cout, result.churnClosesPerSecond);
    std::cout << ",\n    \"churn_worker_accept_counts\": ";
    printIntegerArray(std::cout, result.churnWorkerAcceptCounts);
    std::cout << ",\n    \"churn_worker_skew_ratio\": ";
    printOptional(std::cout, result.churnWorkerSkewRatio);
    std::cout << ",\n    \"churn_connect_p99_us\": ";
    printOptional(std::cout, result.churnConnectP99Us);
    std::cout << ",\n    \"churn_connect_max_us\": ";
    printOptional(std::cout, result.churnConnectMaxUs);
    std::cout << ",\n    \"churn_accept_p99_us\": ";
    printOptional(std::cout, result.churnAcceptP99Us);
    std::cout << ",\n    \"churn_accept_max_us\": ";
    printOptional(std::cout, result.churnAcceptMaxUs);
    std::cout << ",\n    \"churn_close_p99_us\": ";
    printOptional(std::cout, result.churnCloseP99Us);
    std::cout << ",\n    \"churn_close_max_us\": ";
    printOptional(std::cout, result.churnCloseMaxUs);
    std::cout << ",\n    \"churn_schedule_lag_p99_us\": ";
    printOptional(std::cout, result.churnScheduleLagP99Us);
    std::cout << ",\n    \"churn_schedule_lag_max_us\": ";
    printOptional(std::cout, result.churnScheduleLagMaxUs);
    std::cout << ",\n    \"churn_close_reason_counts\": ";
    printCloseReasonCounts(std::cout, result.churnCloseReasonCounts);
    std::cout << ",\n"
              << "    \"requested_bytes\": " << result.requestedBytes << ",\n"
              << "    \"accepted_bytes\": " << result.acceptedBytes << ",\n"
              << "    \"rejected_bytes\": " << result.rejectedBytes << ",\n"
              << "    \"accepted_sends\": " << result.acceptedSends << ",\n"
              << "    \"rejected_sends\": " << result.rejectedSends << ",\n"
              << "    \"overloaded_sends\": " << result.overloadedSends << ",\n"
              << "    \"closed_sends\": " << result.closedSends << ",\n"
              << "    \"owner_unavailable_sends\": " << result.ownerUnavailableSends << ",\n"
              << "    \"output_hard_limit_bytes\": " << result.outputHardLimitBytes << ",\n"
              << "    \"pending_output_peak_bytes\": " << result.pendingOutputPeakBytes << ",\n"
              << "    \"read_pause_observations\": " << result.readPauseObservations << ",\n"
              << "    \"read_resume_observations\": " << result.readResumeObservations << ",\n"
              << "    \"backpressure_recovery_seconds\": ";
    printOptional(std::cout, result.backpressureRecoverySeconds);
    std::cout << ",\n"
              << "    \"high_water_callbacks\": " << result.highWaterCallbacks << "\n"
              << "  }\n"
              << "}\n";
}

int run(const Config& config) {
    gamenet::base::Logger::setLogLevel(gamenet::base::Logger::FATAL);
    gamenet::net::EventLoop loop;
    gamenet::net::TcpServer server(
        &loop,
        gamenet::net::InetAddress(0, true),
        "core-benchmark");
    server.setIocpAcceptDepth(config.iocpAcceptDepth);
    server.setThreadNum(static_cast<int>(config.eventLoopThreads));
    gamenet::net::TcpConnectionBackpressureOptions backpressureOptions;
    backpressureOptions.highWaterMarkBytes = config.highWaterBytes;
    backpressureOptions.lowWaterMarkBytes = std::max<std::size_t>(
        1,
        config.highWaterBytes / 2);
    server.setConnectionBackpressureOptions(backpressureOptions);

    SharedState state;
    Result result;
    result.outputLowWaterBytes = backpressureOptions.lowWaterMarkBytes;
    result.outputHighWaterBytes = backpressureOptions.highWaterMarkBytes;
    result.outputHardLimitBytes = backpressureOptions.hardLimitBytes;
    result.maxInputBufferBytes = backpressureOptions.maxInputBufferBytes;
    const std::string payload(
        config.scenario == "slow-client" ? config.slowBytes : config.payloadBytes,
        'x');

    const auto baseExecutor = loop.executor();
    std::atomic<bool> completionCheckQueued{false};
    std::function<void()> checkCompletion;
    bool connectionMapRetryScheduled = false;
    bool stopRequested = false;
    bool finishing = false;
    std::jthread stopWaiter;

    auto requestStop = [&](bool force) {
        if (!stopRequested) {
            stopRequested = true;
            const auto stopStarted = Clock::now();
            auto stopFuture = server.stopGracefully({
                .drainTimeout = config.timeout,
            });
            stopWaiter = std::jthread(
                [&, stopStarted, stopFuture = std::move(stopFuture)] {
                    const auto stopResult = stopFuture.get();
                    const auto stopFinished = Clock::now();
                    if (!baseExecutor.tryQueue(
                            [&, stopStarted, stopFinished, stopResult] {
                                result.serverStopSeconds =
                                    std::chrono::duration<double>(
                                        stopFinished - stopStarted)
                                        .count();
                                result.serverStopOutcome =
                                    stopOutcomeName(stopResult.outcome);
                                result.serverStopInitialConnections =
                                    stopResult.initialConnectionCount;
                                result.serverStopForcedConnections =
                                    stopResult.forcedConnectionCount;
                                finishing = true;
                                loop.quit();
                            })) {
                        state.fail(
                            "server-stop completion could not reach the base loop");
                        loop.quit();
                    }
                });
        }
        if (force) {
            server.stop();
        }
    };

    auto scheduleCompletionCheck = [&] {
        bool expected = false;
        if (!completionCheckQueued.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
        if (!baseExecutor.tryQueue([&] {
                completionCheckQueued.store(false, std::memory_order_release);
                checkCompletion();
            })) {
            completionCheckQueued.store(false, std::memory_order_release);
            state.fail("benchmark completion check could not reach the base loop");
            loop.quit();
        }
    };

    checkCompletion = [&] {
        loop.assertInLoopThread();
        if (finishing || stopRequested || !state.driverDone()) {
            return;
        }
        if (state.connected() != state.disconnected()) {
            return;
        }
        if (server.connectionCount() != 0) {
            if (!connectionMapRetryScheduled) {
                connectionMapRetryScheduled = true;
                loop.runAfter(1ms, [&] {
                    connectionMapRetryScheduled = false;
                    checkCompletion();
                });
            }
            return;
        }
        result.connectionCloseSeconds = state.clientCloseElapsedSeconds();
        requestStop(false);
    };

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            if (config.scenario == "slow-client") {
                const auto sendResult = connection->trySend(payload);
                state.markSlowSend(
                    sendResult,
                    payload.size(),
                    connection->pendingOutputBytes(),
                    connection->readingPausedByBackpressure());
            }
            state.markConnected(connection->getLoop());
        } else {
            state.markDisconnected();
            scheduleCompletionCheck();
        }
    });
    if (config.scenario == "connection-churn") {
        server.setCloseInfoCallback(
            [&](const gamenet::net::TcpConnectionPtr&,
                const gamenet::net::TcpConnectionCloseInfo& closeInfo) {
                state.markCloseReason(closeInfo.reason);
            });
    }
    if (config.scenario == "echo") {
        server.setMessageCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* buffer) {
                const std::size_t readable = buffer->readableBytes();
                const auto sendResult = connection->trySend(buffer->peek(), readable);
                buffer->retrieve(readable);
                if (sendResult != gamenet::net::TcpSendResult::Accepted) {
                    state.fail("echo response was rejected by TcpConnection admission");
                    connection->forceClose();
                }
            });
    }
    if (config.scenario == "slow-client") {
        server.setHighWaterMarkCallback(
            [&](const gamenet::net::TcpConnectionPtr&, std::size_t) { state.markHighWater(); },
            config.highWaterBytes);
        server.setWriteCompleteCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection) {
                state.markSlowWriteComplete(
                    !connection->readingPausedByBackpressure());
            });
    }

    server.start();
    result.workingSetBefore = sampleWorkingSetBytes();
    const gamenet::net::InetAddress address = server.listenAddress();

    std::thread driver([&] {
        try {
            if (config.scenario == "echo") {
                runEcho(config, address, payload, state, result);
            } else if (config.scenario == "connections") {
                runConnections(config, address, state, result);
            } else if (config.scenario == "connection-churn") {
                runConnectionChurn(config, address, state, result);
            } else {
                runSlowClient(config, address, state, result);
            }
        } catch (const std::exception& error) {
            state.fail(error.what());
        }
        state.markDriverDone();
        scheduleCompletionCheck();
    });

    if (config.preloadBeforeLoop) {
        if (!state.waitForClientsCreated(config.timeout)) {
            state.fail("timed out preloading clients before EventLoop start");
        } else {
            state.beginAcceptDrain();
        }
    }

    loop.runAfter(config.timeout + config.settle + 5s, [&] {
        if (finishing) {
            return;
        }
        state.fail("benchmark overall timeout");
        requestStop(true);
    });
    loop.loop();
    if (stopWaiter.joinable()) {
        stopWaiter.join();
    }
    driver.join();

    result.highWaterCallbacks = state.highWaterCallbacks();
    result.requestedBytes = state.requestedBytes();
    result.acceptedBytes = state.acceptedBytes();
    result.rejectedBytes = state.rejectedBytes();
    result.acceptedSends = state.acceptedSends();
    result.rejectedSends = state.rejectedSends();
    result.overloadedSends = state.overloadedSends();
    result.closedSends = state.closedSends();
    result.ownerUnavailableSends = state.ownerUnavailableSends();
    result.churnCloseReasonCounts = state.closeReasonCounts();
    result.pendingOutputPeakBytes = state.pendingOutputPeakBytes();
    result.readPauseObservations = state.readPauseObservations();
    result.readResumeObservations = state.readResumeObservations();
    if (config.scenario == "slow-client" &&
        (result.requestedBytes != result.acceptedBytes + result.rejectedBytes ||
         result.acceptedSends + result.rejectedSends != config.connections)) {
        state.fail("slow-client admission accounting is inconsistent");
    }
    printResult(config, result, state);
    return state.failure().empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseArgs(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "gamenet_core_benchmark: " << error.what() << '\n';
        printUsage(std::cerr);
        return 2;
    }
}
