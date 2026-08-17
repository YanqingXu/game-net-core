#include "gamenet/broadcast/BroadcastDispatcher.h"
#include "gamenet/broadcast/BroadcastRouter.h"
#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/NetworkMemoryRetention.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/transport/TcpTransportEndpoint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <psapi.h>
#else
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifndef GAMENET_BENCHMARK_BUILD_TYPE
#define GAMENET_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Config {
    std::string scenario{"slow-broadcast-recovery"};
    std::size_t connections{4};
    std::size_t threads{2};
    std::size_t messages{64};
    std::size_t payloadBytes{256U * 1024U};
    std::size_t lowWaterBytes{256U * 1024U};
    std::size_t highWaterBytes{512U * 1024U};
    std::size_t hardLimitBytes{2U * 1024U * 1024U};
    std::size_t serverSendBufferBytes{0};
    std::size_t recoveryThresholdBytes{0};
    std::size_t iocpAcceptDepth{8};
    std::chrono::milliseconds pressureSettle{500};
    std::chrono::milliseconds recoveryStable{250};
    std::chrono::milliseconds timeout{30000};
    std::size_t probeTargetPerSecond{100};
    std::chrono::milliseconds probeDuration{2000};
    std::size_t probeBatchSize{10};
    std::size_t probeConcurrency{4};
    std::size_t probePayloadBytes{32};
    std::chrono::milliseconds probeConnectTimeout{1000};
    std::size_t readerConcurrency{16};
};

bool isMixedProfile(const Config& config) noexcept {
    return config.scenario == "mixed-pressure-recovery";
}

struct ConnectionHandle {
    std::shared_ptr<gamenet::net::TcpConnection> connection;
    std::shared_ptr<gamenet::transport::TcpTransportEndpoint> endpoint;
};

struct AggregateOutputSnapshot {
    std::size_t pendingBytes{};
    std::size_t peakPendingBytes{};
    std::uint64_t rejectedReservations{};
    std::size_t overloadedConnections{};
};

struct AggregateRetentionSnapshot {
    std::size_t inputBufferBytes{};
    std::size_t outputBufferBytes{};
    std::size_t transportReadStorageBytes{};
    std::size_t totalConnectionBytes{};
    std::size_t peakInputBufferBytes{};
    std::size_t peakOutputBufferBytes{};
    std::size_t peakTransportReadStorageBytes{};
    std::uint64_t inputTrimCount{};
    std::uint64_t outputTrimCount{};
};

struct AggregateProgressSnapshot {
    std::size_t scheduledEndpoints{};
    std::size_t acceptedEndpoints{};
    std::size_t droppedEndpoints{};
    std::size_t outstandingTasks{};
    std::size_t outstandingBytes{};
    bool complete{false};
    std::array<
        std::size_t,
        gamenet::broadcast::kBroadcastReasonCount>
        reasonCounts{};
};

struct RejectionSnapshot {
    std::uint64_t connection{};
    std::uint64_t loop{};
    std::uint64_t server{};
    std::uint64_t global{};

    std::uint64_t total() const noexcept {
        return connection + loop + server + global;
    }
};

struct HealthyProbeResult {
    std::uint64_t attempted{};
    std::uint64_t clientConnected{};
    std::uint64_t serverAccepted{};
    std::uint64_t probeSucceeded{};
    std::uint64_t serverClosed{};
    std::uint64_t batches{};
    std::uint64_t connectFailures{};
    std::uint64_t sendFailures{};
    std::uint64_t receiveFailures{};
    std::uint64_t payloadMismatches{};
    double elapsedMs{};
    double attemptsPerSecond{};
    double connectP99Us{};
    double probeP99Us{};
    double scheduleLagP99Us{};

    std::uint64_t totalFailures() const noexcept {
        return connectFailures +
            sendFailures +
            receiveFailures +
            payloadMismatches;
    }
};

struct RecoveryReaderResult {
    std::uint64_t workers{};
    std::uint64_t assignedSockets{};
    std::uint64_t closedSockets{};
};

struct Result {
    std::uint64_t workingSetBeforeBytes{};
    std::uint64_t workingSetPressureBytes{};
    std::uint64_t workingSetRecoveryBytes{};
    std::uint64_t workingSetAfterBytes{};
    std::uint64_t workingSetPeakBytes{};
    std::int64_t workingSetRecoveryDeltaBytes{};
    double pressureElapsedMs{};
    double recoveryElapsedMs{};
    std::uint64_t clientReceivedBytes{};

    AggregateOutputSnapshot pressureOutput;
    AggregateOutputSnapshot recoveryOutput;
    AggregateRetentionSnapshot pressureRetention;
    AggregateRetentionSnapshot recoveryRetention;
    AggregateProgressSnapshot terminal;
    gamenet::broadcast::DispatchOutstandingSnapshot pressureBroadcast;
    gamenet::broadcast::DispatchOutstandingSnapshot recoveryBroadcast;
    gamenet::net::NetworkFixedStorageRetentionSnapshot fixedBaseline;
    gamenet::net::NetworkFixedStorageRetentionSnapshot fixedPressure;
    gamenet::net::NetworkFixedStorageRetentionSnapshot fixedRecovery;
    gamenet::net::NetworkFixedStorageRetentionSnapshot fixedAfterTeardown;
    RejectionSnapshot rejections;
    HealthyProbeResult healthyProbe;
    RecoveryReaderResult recoveryReaders;

    bool pendingWithinLimit{};
    bool broadcastWithinLimit{};
    bool terminalAccounted{};
    bool clientDeliveryAccounted{};
    bool rejectionAttributed{};
    bool overloadObserved{};
    bool recoveryStable{};
    bool recoveryRetainedWithinTarget{};
    bool fixedStorageCoherent{};
    bool teardownReleased{};
    bool healthyProbeAccounted{true};
    bool healthyProbeZeroFailures{true};
    bool healthyProbeClosed{true};
    bool healthyProbePaced{true};
    bool recoveryReaderPoolAccounted{true};

    bool passed() const noexcept {
        return pendingWithinLimit &&
            broadcastWithinLimit &&
            terminalAccounted &&
            clientDeliveryAccounted &&
            rejectionAttributed &&
            overloadObserved &&
            recoveryStable &&
            recoveryRetainedWithinTarget &&
            fixedStorageCoherent &&
            teardownReleased &&
            healthyProbeAccounted &&
            healthyProbeZeroFailures &&
            healthyProbeClosed &&
            healthyProbePaced &&
            recoveryReaderPoolAccounted;
    }
};

class HelpRequested final : public std::exception {};

[[noreturn]] void usageError(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

std::uint64_t parseUnsigned(
    std::string_view text,
    std::string_view option) {
    if (text.empty()) {
        usageError(std::string(option) + " requires a value");
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            usageError(
                std::string(option) +
                " requires an unsigned integer");
        }
        const auto digit =
            static_cast<std::uint64_t>(ch - '0');
        if (value >
            ((std::numeric_limits<std::uint64_t>::max)() - digit) /
                10U) {
            usageError(std::string(option) + " is too large");
        }
        value = value * 10U + digit;
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
        usageError(
            std::string(option) + " must be between " +
            std::to_string(minimum) + " and " +
            std::to_string(maximum));
    }
    return static_cast<std::size_t>(value);
}

std::size_t checkedMultiply(
    std::size_t left,
    std::size_t right,
    std::string_view label) {
    if (left != 0 &&
        right >
            (std::numeric_limits<std::size_t>::max)() / left) {
        throw std::invalid_argument(
            std::string(label) + " exceeds addressable size");
    }
    return left * right;
}

void printUsage(std::ostream& output) {
    output
        << "Usage: gamenet_capacity_profile [options]\n"
        << "  --scenario slow-broadcast-recovery|mixed-pressure-recovery\n"
        << "  --connections N          real slow-reader TCP clients\n"
        << "  --threads N              TcpServer worker loops\n"
        << "  --messages N             broadcasts issued under pressure\n"
        << "  --payload-bytes N        bytes per shared broadcast payload\n"
        << "  --low-water-bytes N      connection recovery watermark\n"
        << "  --high-water-bytes N     connection read-pause watermark\n"
        << "  --hard-limit-bytes N     connection pending-output limit\n"
        << "  --server-send-buffer-bytes N (0 keeps the OS default)\n"
        << "  --recovery-threshold-bytes N\n"
        << "  --pressure-settle-ms N\n"
        << "  --recovery-stable-ms N\n"
        << "  --timeout-ms N\n"
        << "  --iocp-accept-depth N\n"
        << "  --probe-rate N            mixed healthy attempts/second\n"
        << "  --probe-duration-ms N     mixed healthy paced duration\n"
        << "  --probe-batch-size N      mixed maximum live probe batch\n"
        << "  --probe-concurrency N     mixed persistent connector workers\n"
        << "  --probe-payload-bytes N   mixed exact echo payload\n"
        << "  --probe-connect-timeout-ms N\n"
        << "  --reader-concurrency N    mixed recovery reader ceiling\n";
}

Config parseArgs(int argc, char* argv[]) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help" || option == "-h") {
            throw HelpRequested{};
        }
        if (index + 1 >= argc) {
            usageError(std::string(option) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (option == "--scenario") {
            config.scenario = value;
        } else if (option == "--connections") {
            config.connections =
                parseSize(value, option, 1, 10000);
        } else if (option == "--threads") {
            config.threads = parseSize(value, option, 1, 64);
        } else if (option == "--messages") {
            config.messages =
                parseSize(value, option, 1, 100000);
        } else if (option == "--payload-bytes") {
            config.payloadBytes =
                parseSize(value, option, 1, 16U * 1024U * 1024U);
        } else if (option == "--low-water-bytes") {
            config.lowWaterBytes =
                parseSize(value, option, 1, 1024U * 1024U * 1024U);
        } else if (option == "--high-water-bytes") {
            config.highWaterBytes =
                parseSize(value, option, 1, 1024U * 1024U * 1024U);
        } else if (option == "--hard-limit-bytes") {
            config.hardLimitBytes =
                parseSize(value, option, 1, 1024U * 1024U * 1024U);
        } else if (option == "--server-send-buffer-bytes") {
            config.serverSendBufferBytes =
                parseSize(
                    value,
                    option,
                    0,
                    static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)()));
        } else if (option == "--recovery-threshold-bytes") {
            config.recoveryThresholdBytes =
                parseSize(value, option, 0, 1024U * 1024U * 1024U);
        } else if (option == "--pressure-settle-ms") {
            config.pressureSettle =
                std::chrono::milliseconds(
                    parseSize(value, option, 0, 60000));
        } else if (option == "--recovery-stable-ms") {
            config.recoveryStable =
                std::chrono::milliseconds(
                    parseSize(value, option, 1, 60000));
        } else if (option == "--timeout-ms") {
            config.timeout =
                std::chrono::milliseconds(
                    parseSize(value, option, 100, 300000));
        } else if (option == "--iocp-accept-depth") {
            config.iocpAcceptDepth =
                parseSize(value, option, 1, 64);
        } else if (option == "--probe-rate") {
            config.probeTargetPerSecond =
                parseSize(value, option, 1, 100000);
        } else if (option == "--probe-duration-ms") {
            config.probeDuration =
                std::chrono::milliseconds(
                    parseSize(value, option, 100, 600000));
        } else if (option == "--probe-batch-size") {
            config.probeBatchSize =
                parseSize(value, option, 1, 10000);
        } else if (option == "--probe-concurrency") {
            config.probeConcurrency =
                parseSize(value, option, 1, 1024);
        } else if (option == "--probe-payload-bytes") {
            config.probePayloadBytes =
                parseSize(value, option, 1, 64U * 1024U);
        } else if (option == "--probe-connect-timeout-ms") {
            config.probeConnectTimeout =
                std::chrono::milliseconds(
                    parseSize(value, option, 1, 30000));
        } else if (option == "--reader-concurrency") {
            config.readerConcurrency =
                parseSize(value, option, 1, 1024);
        } else {
            usageError(
                std::string("unknown option: ") +
                std::string(option));
        }
    }

    if (config.scenario != "slow-broadcast-recovery" &&
        config.scenario != "mixed-pressure-recovery") {
        usageError(
            "--scenario must be slow-broadcast-recovery or "
            "mixed-pressure-recovery");
    }
    if (config.lowWaterBytes > config.highWaterBytes ||
        config.highWaterBytes > config.hardLimitBytes) {
        usageError(
            "connection watermarks must satisfy low <= high <= hard");
    }
    if (config.recoveryThresholdBytes >
        config.lowWaterBytes) {
        usageError(
            "--recovery-threshold-bytes must not exceed "
            "--low-water-bytes");
    }
    (void)checkedMultiply(
        checkedMultiply(
            config.connections,
            config.messages,
            "logical broadcast endpoint count"),
        config.payloadBytes,
        "logical broadcast bytes");
    if (isMixedProfile(config)) {
        if (config.probeConcurrency > config.probeBatchSize) {
            usageError(
                "--probe-concurrency must not exceed "
                "--probe-batch-size");
        }
        if (config.probeDuration > config.timeout ||
            config.probeConnectTimeout > config.timeout) {
            usageError(
                "probe duration and connect timeout must not exceed "
                "--timeout-ms");
        }
        const auto attempts =
            static_cast<std::uint64_t>(
                config.probeTargetPerSecond) *
            static_cast<std::uint64_t>(
                config.probeDuration.count()) /
            1000U;
        if (attempts == 0) {
            usageError(
                "probe rate and duration produce zero attempts");
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
        throw std::runtime_error(
            "GetProcessMemoryInfo failed");
    }
    return static_cast<std::uint64_t>(
        counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        throw std::runtime_error(
            "failed to read /proc/self/statm");
    }
    (void)totalPages;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        throw std::runtime_error(
            "sysconf(_SC_PAGESIZE) failed");
    }
    return residentPages *
        static_cast<std::uint64_t>(pageSize);
#endif
}

class WorkingSetSampler {
public:
    explicit WorkingSetSampler(std::uint64_t initial)
        : peak_(initial) {
        thread_ = std::thread([this] {
            while (running_.load(std::memory_order_acquire)) {
                try {
                    observe(sampleWorkingSetBytes());
                } catch (...) {
                    failed_.store(true, std::memory_order_release);
                    running_.store(false, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }

    WorkingSetSampler(const WorkingSetSampler&) = delete;
    WorkingSetSampler& operator=(const WorkingSetSampler&) = delete;

    ~WorkingSetSampler() {
        if (thread_.joinable()) {
            try {
                (void)stop();
            } catch (...) {
            }
        }
    }

    std::uint64_t stop(std::uint64_t finalSample = 0) {
        observe(finalSample);
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (failed_.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "working-set sampler failed");
        }
        return peak_.load(std::memory_order_acquire);
    }

private:
    void observe(std::uint64_t sample) noexcept {
        auto peak = peak_.load(std::memory_order_relaxed);
        while (sample > peak &&
               !peak_.compare_exchange_weak(
                   peak,
                   sample,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    std::atomic<bool> running_{true};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> peak_;
    std::thread thread_;
};

std::string socketFailure(std::string_view operation) {
    const int error = gamenet::net::sockets::lastError();
    return std::string(operation) + ": " +
        gamenet::net::sockets::errorMessage(error);
}

struct ClientDrainResult {
    std::uint64_t bytes{};
    bool closed{false};
};

class ClientSocket {
public:
    ClientSocket() = default;
    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    ClientSocket(ClientSocket&& other) noexcept
        : fd_(std::exchange(
              other.fd_,
              gamenet::net::kInvalidSocket)) {}

    ClientSocket& operator=(ClientSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(
                other.fd_,
                gamenet::net::kInvalidSocket);
        }
        return *this;
    }

    ~ClientSocket() { close(); }

    static ClientSocket connectTo(
        const gamenet::net::InetAddress& address,
        std::chrono::milliseconds timeout) {
        gamenet::net::sockets::ensureInitialized();
        ClientSocket socket;
        socket.fd_ =
            ::socket(address.family(), SOCK_STREAM, IPPROTO_TCP);
        if (!gamenet::net::sockets::isValid(socket.fd_)) {
            throw std::runtime_error(socketFailure("socket"));
        }
        socket.configure(timeout, true);
        if (::connect(
                socket.fd_,
                address.getSockAddr(),
                address.getSockAddrLen()) != 0) {
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
            gamenet::net::sockets::createNonblocking(
                address.family());
        if (!gamenet::net::sockets::isValid(socket.fd_)) {
            throw std::runtime_error(socketFailure("socket"));
        }
        socket.configure(timeout, false);
        if (::connect(
                socket.fd_,
                address.getSockAddr(),
                address.getSockAddrLen()) != 0) {
            const int connectError =
                gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isInProgress(
                    connectError) &&
                !gamenet::net::sockets::isWouldBlock(
                    connectError)) {
                throw std::runtime_error(
                    "connect: " +
                    gamenet::net::sockets::errorMessage(
                        connectError));
            }
            socket.waitForConnect(timeout);
        }
        socket.setBlocking();
        return socket;
    }

    void sendAll(std::string_view payload) {
        std::size_t sent = 0;
        while (sent < payload.size()) {
            const int count = static_cast<int>((std::min)(
                payload.size() - sent,
                static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())));
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const int written = ::send(
                fd_,
                payload.data() + sent,
                count,
                flags);
            if (written <= 0) {
                throw std::runtime_error(socketFailure("send"));
            }
            sent += static_cast<std::size_t>(written);
        }
    }

    void receiveExact(std::string& destination) {
        std::size_t received = 0;
        while (received < destination.size()) {
            const int count = static_cast<int>((std::min)(
                destination.size() - received,
                static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())));
            const int read = ::recv(
                fd_,
                destination.data() + received,
                count,
                0);
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

    void makeNonblocking() {
#ifdef _WIN32
        u_long nonBlocking = 1;
        if (::ioctlsocket(
                fd_,
                FIONBIO,
                &nonBlocking) == SOCKET_ERROR) {
            throw std::runtime_error(
                socketFailure("ioctlsocket(FIONBIO=1)"));
        }
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(
                fd_,
                F_SETFL,
                flags | O_NONBLOCK) < 0) {
            throw std::runtime_error(
                socketFailure("fcntl(O_NONBLOCK=1)"));
        }
#endif
    }

    ClientDrainResult drainAvailable(
        std::array<char, 64U * 1024U>& buffer,
        bool shutdownExpected) {
        for (;;) {
            const int received = ::recv(
                fd_,
                buffer.data(),
                static_cast<int>(buffer.size()),
                0);
            if (received > 0) {
                return {
                    .bytes = static_cast<std::uint64_t>(
                        received),
                    .closed = false,
                };
            }
            if (received == 0) {
                return {.bytes = 0, .closed = true};
            }
            const int error =
                gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isInterrupted(error)) {
                continue;
            }
            if (gamenet::net::sockets::isWouldBlock(error)) {
                return {};
            }
            if (shutdownExpected) {
                return {.bytes = 0, .closed = true};
            }
            throw std::runtime_error(
                std::string("recv: ") +
                gamenet::net::sockets::errorMessage(error));
        }
    }

    std::uint64_t drainUntilClosed(
        const std::atomic<bool>& stopIssued) {
        std::array<char, 64U * 1024U> bytes{};
        std::uint64_t total = 0;
        for (;;) {
            const int received = ::recv(
                fd_,
                bytes.data(),
                static_cast<int>(bytes.size()),
                0);
            if (received > 0) {
                total += static_cast<std::uint64_t>(received);
                continue;
            }
            if (received == 0) {
                return total;
            }
            const int error = gamenet::net::sockets::lastError();
            if (gamenet::net::sockets::isInterrupted(error)) {
                continue;
            }
            if (stopIssued.load(std::memory_order_acquire)) {
                return total;
            }
            throw std::runtime_error(
                std::string("recv: ") +
                gamenet::net::sockets::errorMessage(error));
        }
    }

private:
    void waitForConnect(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (true) {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                throw std::runtime_error(
                    "connect: deadline expired");
            }
#ifdef _WIN32
            const auto remainingMicroseconds =
                std::chrono::duration_cast<
                    std::chrono::microseconds>(remaining);
            timeval timeoutValue{};
            timeoutValue.tv_sec =
                static_cast<decltype(timeoutValue.tv_sec)>(
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
            constexpr int descriptorCount = 0;
            const int ready = ::select(
                descriptorCount,
                nullptr,
                &writable,
                &failed,
                &timeoutValue);
#else
            const auto remainingMilliseconds =
                std::chrono::ceil<std::chrono::milliseconds>(
                    remaining);
            const auto boundedTimeout = std::min(
                remainingMilliseconds.count(),
                static_cast<decltype(
                    remainingMilliseconds.count())>(
                    std::numeric_limits<int>::max()));
            pollfd descriptor{
                .fd = fd_,
                .events = POLLOUT,
                .revents = 0,
            };
            const int ready = ::poll(
                &descriptor,
                1,
                static_cast<int>(boundedTimeout));
#endif
            if (ready > 0) {
#ifndef _WIN32
                if ((descriptor.revents & POLLNVAL) != 0) {
                    throw std::runtime_error(
                        "poll(connect): invalid socket");
                }
#endif
                const int socketError =
                    gamenet::net::sockets::getSocketError(fd_);
                if (socketError != 0) {
                    throw std::runtime_error(
                        "connect: " +
                        gamenet::net::sockets::errorMessage(
                            socketError));
                }
                return;
            }
            if (ready == 0) {
                throw std::runtime_error(
                    "connect: deadline expired");
            }
            const int waitError =
                gamenet::net::sockets::lastError();
            if (!gamenet::net::sockets::isInterrupted(
                    waitError)) {
                throw std::runtime_error(
#ifdef _WIN32
                    "select(connect): " +
#else
                    "poll(connect): " +
#endif
                    gamenet::net::sockets::errorMessage(
                        waitError));
            }
        }
    }

    void setBlocking() {
#ifdef _WIN32
        u_long nonBlocking = 0;
        if (::ioctlsocket(
                fd_,
                FIONBIO,
                &nonBlocking) == SOCKET_ERROR) {
            throw std::runtime_error(
                socketFailure("ioctlsocket(FIONBIO=0)"));
        }
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(
                fd_,
                F_SETFL,
                flags & ~O_NONBLOCK) < 0) {
            throw std::runtime_error(
                socketFailure("fcntl(O_NONBLOCK=0)"));
        }
#endif
    }

    void configure(
        std::chrono::milliseconds timeout,
        bool slowReader) {
        const int noDelay = 1;
        if (::setsockopt(
                fd_,
                IPPROTO_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay),
                static_cast<socklen_t>(sizeof(noDelay))) != 0) {
            throw std::runtime_error(
                socketFailure("setsockopt(TCP_NODELAY)"));
        }

#ifdef _WIN32
        const DWORD timeoutValue =
            static_cast<DWORD>(timeout.count());
#else
        timeval timeoutValue{};
        timeoutValue.tv_sec =
            static_cast<time_t>(timeout.count() / 1000);
        timeoutValue.tv_usec =
            static_cast<suseconds_t>(
                (timeout.count() % 1000) * 1000);
#endif
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
            throw std::runtime_error(
                socketFailure("setsockopt(socket timeout)"));
        }

        if (slowReader) {
            const int receiveBufferBytes = 4096;
            if (::setsockopt(
                    fd_,
                    SOL_SOCKET,
                    SO_RCVBUF,
                    reinterpret_cast<const char*>(
                        &receiveBufferBytes),
                    static_cast<socklen_t>(
                        sizeof(receiveBufferBytes))) != 0) {
                throw std::runtime_error(
                    socketFailure("setsockopt(SO_RCVBUF)"));
            }
        }
    }

    void close() noexcept {
        if (gamenet::net::sockets::isValid(fd_)) {
            gamenet::net::sockets::close(fd_);
            fd_ = gamenet::net::kInvalidSocket;
        }
    }

    gamenet::net::SocketFd fd_{
        gamenet::net::kInvalidSocket};
};

enum class ProbeFailure {
    None,
    Connect,
    Send,
    Receive,
    PayloadMismatch,
};

struct ProbeAttempt {
    bool clientConnected{false};
    bool succeeded{false};
    ProbeFailure failure{ProbeFailure::None};
    double connectUs{};
    double probeUs{};
};

class HealthyProbePool {
public:
    HealthyProbePool(
        const Config& config,
        gamenet::net::InetAddress address)
        : address_(std::move(address)),
          connectTimeout_(config.probeConnectTimeout),
          payload_(config.probePayloadBytes, 'h'),
          workerCount_(config.probeConcurrency) {
        workers_.reserve(workerCount_);
        try {
            for (std::size_t index = 0;
                 index < workerCount_;
                 ++index) {
                workers_.emplace_back(
                    [this] { workerMain(); });
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            workAvailable_.notify_all();
            for (auto& worker : workers_) {
                worker.join();
            }
            throw;
        }
    }

    HealthyProbePool(const HealthyProbePool&) = delete;
    HealthyProbePool& operator=(const HealthyProbePool&) = delete;

    ~HealthyProbePool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    std::vector<ProbeAttempt> runBatch(
        std::size_t attemptCount) {
        {
            std::lock_guard lock(mutex_);
            slots_.assign(attemptCount, {});
            attemptCount_ = attemptCount;
            nextIndex_.store(0, std::memory_order_relaxed);
            completedWorkers_ = 0;
            ++generation_;
        }
        workAvailable_.notify_all();
        {
            std::unique_lock lock(mutex_);
            batchComplete_.wait(lock, [&] {
                return completedWorkers_ == workerCount_;
            });
        }
        return slots_;
    }

private:
    void workerMain() {
        std::uint64_t observedGeneration = 0;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                workAvailable_.wait(lock, [&] {
                    return stopping_ ||
                        generation_ != observedGeneration;
                });
                if (stopping_) {
                    return;
                }
                observedGeneration = generation_;
            }

            while (true) {
                const std::size_t index =
                    nextIndex_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                if (index >= attemptCount_) {
                    break;
                }

                ProbeAttempt attempt;
                ProbeFailure stage = ProbeFailure::Connect;
                ClientSocket socket;
                try {
                    const auto connectStarted = Clock::now();
                    socket = ClientSocket::connectToWithDeadline(
                        address_,
                        connectTimeout_);
                    const auto connectFinished = Clock::now();
                    attempt.clientConnected = true;
                    attempt.connectUs =
                        std::chrono::duration<
                            double,
                            std::micro>(
                                connectFinished -
                                connectStarted)
                            .count();

                    stage = ProbeFailure::Send;
                    const auto probeStarted = Clock::now();
                    socket.sendAll(payload_);
                    stage = ProbeFailure::Receive;
                    std::string response(payload_.size(), '\0');
                    socket.receiveExact(response);
                    attempt.probeUs =
                        std::chrono::duration<
                            double,
                            std::micro>(
                                Clock::now() - probeStarted)
                            .count();
                    if (response != payload_) {
                        attempt.failure =
                            ProbeFailure::PayloadMismatch;
                    } else {
                        attempt.succeeded = true;
                    }
                } catch (const std::exception&) {
                    attempt.failure = stage;
                }
                socket.closeAbortively();
                slots_[index] = attempt;
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
    std::chrono::milliseconds connectTimeout_;
    std::string payload_;
    std::size_t workerCount_;
    std::vector<ProbeAttempt> slots_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> nextIndex_{0};
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable batchComplete_;
    std::size_t attemptCount_{0};
    std::size_t completedWorkers_{0};
    std::uint64_t generation_{0};
    bool stopping_{false};
};

class SharedState {
public:
    void publish(
        std::vector<ConnectionHandle> handles,
        std::vector<std::shared_ptr<
            gamenet::broadcast::DispatchProgress>> progress) {
        {
            std::lock_guard lock(mutex_);
            handles_ = std::move(handles);
            progress_ = std::move(progress);
            published_ = true;
        }
        cv_.notify_all();
    }

    bool waitForPublished(
        std::chrono::milliseconds timeout,
        std::vector<ConnectionHandle>* handles,
        std::vector<std::shared_ptr<
            gamenet::broadcast::DispatchProgress>>* progress) {
        std::unique_lock lock(mutex_);
        const bool ready = cv_.wait_for(lock, timeout, [&] {
            return published_ || !failure_.empty();
        });
        if (!ready || !published_) {
            return false;
        }
        *handles = std::move(handles_);
        *progress = std::move(progress_);
        return true;
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

    bool classifyConnected(
        const gamenet::net::TcpConnection* connection,
        std::size_t slowConnectionLimit) {
        const auto ordinal =
            connectedClaims_.fetch_add(
                1,
                std::memory_order_acq_rel);
        if (ordinal < slowConnectionLimit) {
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            probeConnections_.insert(connection);
            ++probeAccepted_;
        }
        cv_.notify_all();
        return true;
    }

    void markDisconnected(
        const gamenet::net::TcpConnection* connection) noexcept {
        bool probe = false;
        {
            std::lock_guard lock(mutex_);
            probe = probeConnections_.erase(connection) != 0;
            if (probe) {
                ++probeClosed_;
            }
        }
        if (!probe) {
            disconnected_.fetch_add(
                1,
                std::memory_order_release);
        }
        cv_.notify_all();
    }

    std::size_t disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }

    bool waitForProbeAccepted(
        std::size_t expected,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return probeAccepted_ >= expected ||
                !failure_.empty();
        }) && probeAccepted_ >= expected;
    }

    bool waitForProbeClosed(
        std::size_t expected,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return probeClosed_ >= expected ||
                !failure_.empty();
        }) && probeClosed_ >= expected;
    }

    std::pair<std::size_t, std::size_t>
    probeConnectionCounts() const {
        std::lock_guard lock(mutex_);
        return {probeAccepted_, probeClosed_};
    }

    void markDriverDone() noexcept {
        driverDone_.store(true, std::memory_order_release);
    }

    bool driverDone() const noexcept {
        return driverDone_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<ConnectionHandle> handles_;
    std::vector<std::shared_ptr<
        gamenet::broadcast::DispatchProgress>> progress_;
    std::string failure_;
    bool published_{false};
    std::unordered_set<const gamenet::net::TcpConnection*>
        probeConnections_;
    std::size_t probeAccepted_{0};
    std::size_t probeClosed_{0};
    std::atomic<std::size_t> connectedClaims_{0};
    std::atomic<std::size_t> disconnected_{0};
    std::atomic<bool> driverDone_{false};
};

double nearestRankP99(std::vector<double>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const auto rank = static_cast<std::size_t>(
        std::ceil(0.99 * static_cast<double>(samples.size())));
    return samples[(std::max)(std::size_t{1}, rank) - 1];
}

void runHealthyProbes(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const std::uint64_t expectedAttempts =
        static_cast<std::uint64_t>(
            config.probeTargetPerSecond) *
        static_cast<std::uint64_t>(
            config.probeDuration.count()) /
        1000U;
    std::vector<double> connectSamples;
    std::vector<double> probeSamples;
    std::vector<double> scheduleLagSamples;
    const auto started = Clock::now();

    {
        HealthyProbePool pool(config, address);
        while (result.healthyProbe.attempted <
               expectedAttempts) {
            const auto remaining =
                expectedAttempts -
                result.healthyProbe.attempted;
            const std::size_t batchSize =
                static_cast<std::size_t>((std::min)(
                    static_cast<std::uint64_t>(
                        config.probeBatchSize),
                    remaining));
            const auto scheduledAttempts =
                result.healthyProbe.attempted +
                batchSize;
            const double scheduleFraction =
                static_cast<double>(scheduledAttempts) /
                static_cast<double>(expectedAttempts);
            const auto scheduledAt =
                started +
                std::chrono::duration_cast<
                    Clock::duration>(
                        std::chrono::duration<double>(
                            std::chrono::duration<double>(
                                config.probeDuration)
                                .count() *
                            scheduleFraction));
            std::this_thread::sleep_until(scheduledAt);

            auto attempts = pool.runBatch(batchSize);
            result.healthyProbe.attempted =
                scheduledAttempts;
            ++result.healthyProbe.batches;
            for (const auto& attempt : attempts) {
                if (attempt.clientConnected) {
                    ++result.healthyProbe.clientConnected;
                    connectSamples.push_back(
                        attempt.connectUs);
                }
                if (attempt.succeeded) {
                    ++result.healthyProbe.probeSucceeded;
                    probeSamples.push_back(attempt.probeUs);
                    continue;
                }
                switch (attempt.failure) {
                case ProbeFailure::Connect:
                    ++result.healthyProbe.connectFailures;
                    break;
                case ProbeFailure::Send:
                    ++result.healthyProbe.sendFailures;
                    break;
                case ProbeFailure::Receive:
                    ++result.healthyProbe.receiveFailures;
                    break;
                case ProbeFailure::PayloadMismatch:
                    ++result.healthyProbe.payloadMismatches;
                    break;
                case ProbeFailure::None:
                    ++result.healthyProbe.receiveFailures;
                    break;
                }
            }

            if (!state.waitForProbeAccepted(
                    static_cast<std::size_t>(
                        result.healthyProbe.clientConnected),
                    config.timeout)) {
                throw std::runtime_error(
                    "timed out waiting for healthy probe accepts");
            }
            if (!state.waitForProbeClosed(
                    static_cast<std::size_t>(
                        result.healthyProbe.clientConnected),
                    config.timeout)) {
                throw std::runtime_error(
                    "timed out waiting for healthy probe closes");
            }
            scheduleLagSamples.push_back((std::max)(
                0.0,
                std::chrono::duration<
                    double,
                    std::micro>(
                        Clock::now() - scheduledAt)
                    .count()));
        }
    }

    const auto finished = Clock::now();
    result.healthyProbe.elapsedMs =
        std::chrono::duration<double, std::milli>(
            finished - started)
            .count();
    if (result.healthyProbe.elapsedMs <= 0.0) {
        throw std::runtime_error(
            "healthy probe clock did not advance");
    }
    const auto [serverAccepted, serverClosed] =
        state.probeConnectionCounts();
    result.healthyProbe.serverAccepted = serverAccepted;
    result.healthyProbe.serverClosed = serverClosed;
    result.healthyProbe.attemptsPerSecond =
        static_cast<double>(result.healthyProbe.attempted) /
        (result.healthyProbe.elapsedMs / 1000.0);
    result.healthyProbe.connectP99Us =
        nearestRankP99(connectSamples);
    result.healthyProbe.probeP99Us =
        nearestRankP99(probeSamples);
    result.healthyProbe.scheduleLagP99Us =
        nearestRankP99(scheduleLagSamples);
}

AggregateOutputSnapshot aggregateOutput(
    const std::vector<ConnectionHandle>& handles) {
    AggregateOutputSnapshot aggregate;
    for (const auto& handle : handles) {
        const auto snapshot =
            handle.connection->outputMemorySnapshot();
        aggregate.pendingBytes += snapshot.pendingBytes;
        aggregate.peakPendingBytes +=
            snapshot.peakPendingBytes;
        aggregate.rejectedReservations +=
            snapshot.rejectedReservations;
        if (snapshot.overloaded) {
            ++aggregate.overloadedConnections;
        }
    }
    return aggregate;
}

AggregateRetentionSnapshot aggregateRetention(
    const std::vector<ConnectionHandle>& handles,
    std::chrono::milliseconds timeout) {
    struct OwnerBatch {
        gamenet::net::EventLoopExecutor executor;
        std::vector<gamenet::net::TcpConnectionPtr> connections;
    };

    std::unordered_map<std::uint64_t, std::size_t> batchIndexes;
    std::vector<OwnerBatch> batches;
    for (const auto& handle : handles) {
        const auto executor = handle.endpoint->ownerExecutor();
        const auto [found, inserted] = batchIndexes.try_emplace(
            executor.id(),
            batches.size());
        if (inserted) {
            batches.push_back({.executor = executor});
        }
        batches[found->second].connections.push_back(
            handle.connection);
    }

    std::vector<std::future<AggregateRetentionSnapshot>> futures;
    futures.reserve(batches.size());
    for (auto& batch : batches) {
        auto promise = std::make_shared<
            std::promise<AggregateRetentionSnapshot>>();
        futures.push_back(promise->get_future());
        const auto posted = batch.executor.post(
            [connections = std::move(batch.connections), promise] {
                AggregateRetentionSnapshot aggregate;
                try {
                    for (const auto& connection : connections) {
                        const auto snapshot =
                            connection->memoryRetentionSnapshot();
                        aggregate.inputBufferBytes +=
                            snapshot.inputBuffer.retainedCapacityBytes;
                        aggregate.outputBufferBytes +=
                            snapshot.outputBuffer.retainedCapacityBytes;
                        aggregate.transportReadStorageBytes +=
                            snapshot.transportReadStorageBytes;
                        aggregate.totalConnectionBytes +=
                            snapshot.totalRetainedBytes;
                        aggregate.peakInputBufferBytes +=
                            snapshot.inputBuffer.peakRetainedCapacityBytes;
                        aggregate.peakOutputBufferBytes +=
                            snapshot.outputBuffer.peakRetainedCapacityBytes;
                        aggregate.peakTransportReadStorageBytes +=
                            snapshot.peakTransportReadStorageBytes;
                        aggregate.inputTrimCount +=
                            snapshot.inputBuffer.trimCount;
                        aggregate.outputTrimCount +=
                            snapshot.outputBuffer.trimCount;
                    }
                    promise->set_value(aggregate);
                } catch (...) {
                    promise->set_exception(
                        std::current_exception());
                }
            });
        if (posted != gamenet::net::PostResult::Accepted) {
            throw std::runtime_error(
                "owner rejected retained-memory snapshot batch");
        }
    }

    AggregateRetentionSnapshot aggregate;
    for (auto& future : futures) {
        if (future.wait_for(timeout) !=
            std::future_status::ready) {
            throw std::runtime_error(
                "timed out waiting for retained-memory snapshot");
        }
        const auto snapshot = future.get();
        aggregate.inputBufferBytes += snapshot.inputBufferBytes;
        aggregate.outputBufferBytes += snapshot.outputBufferBytes;
        aggregate.transportReadStorageBytes +=
            snapshot.transportReadStorageBytes;
        aggregate.totalConnectionBytes +=
            snapshot.totalConnectionBytes;
        aggregate.peakInputBufferBytes +=
            snapshot.peakInputBufferBytes;
        aggregate.peakOutputBufferBytes +=
            snapshot.peakOutputBufferBytes;
        aggregate.peakTransportReadStorageBytes +=
            snapshot.peakTransportReadStorageBytes;
        aggregate.inputTrimCount += snapshot.inputTrimCount;
        aggregate.outputTrimCount += snapshot.outputTrimCount;
    }
    return aggregate;
}

AggregateProgressSnapshot aggregateProgress(
    const std::vector<std::shared_ptr<
        gamenet::broadcast::DispatchProgress>>& progress) {
    AggregateProgressSnapshot aggregate;
    aggregate.complete = !progress.empty();
    for (const auto& item : progress) {
        const auto snapshot = item->snapshot();
        aggregate.scheduledEndpoints +=
            snapshot.scheduledEndpoints;
        aggregate.acceptedEndpoints +=
            snapshot.acceptedEndpoints;
        aggregate.droppedEndpoints +=
            snapshot.droppedEndpoints;
        aggregate.outstandingTasks +=
            snapshot.outstandingTasks;
        aggregate.outstandingBytes +=
            snapshot.outstandingBytes;
        aggregate.complete =
            aggregate.complete && snapshot.complete;
        for (std::size_t index = 0;
             index < aggregate.reasonCounts.size();
             ++index) {
            aggregate.reasonCounts[index] +=
                snapshot.reasonCounts[index];
        }
    }
    return aggregate;
}

bool waitForTerminalProgress(
    const std::vector<std::shared_ptr<
        gamenet::broadcast::DispatchProgress>>& progress,
    std::chrono::milliseconds timeout,
    AggregateProgressSnapshot* terminal) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        *terminal = aggregateProgress(progress);
        if (terminal->complete) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    *terminal = aggregateProgress(progress);
    return terminal->complete;
}

RejectionSnapshot collectRejections(
    const AggregateOutputSnapshot& output,
    const gamenet::net::TcpServerOutputMemoryStats& serverStats) {
    RejectionSnapshot rejections{
        .connection = output.rejectedReservations,
        .server = serverStats.server.rejectedReservations,
    };
    for (const auto& loop : serverStats.loops) {
        rejections.loop += loop.rejectedReservations;
    }
    if (serverStats.global) {
        rejections.global =
            serverStats.global->rejectedReservations;
    }
    return rejections;
}

std::string makePayload(
    std::size_t sequence,
    std::size_t bytes) {
    std::string payload =
        "capacity-sequence-" + std::to_string(sequence) + ":";
    if (payload.size() > bytes) {
        payload.resize(bytes);
        return payload;
    }
    payload.append(
        bytes - payload.size(),
        static_cast<char>('a' + sequence % 26U));
    return payload;
}

std::int64_t signedDelta(
    std::uint64_t current,
    std::uint64_t baseline) noexcept {
    if (current >= baseline) {
        return static_cast<std::int64_t>(
            current - baseline);
    }
    return -static_cast<std::int64_t>(
        baseline - current);
}

void finalizeChecks(
    const Config& config,
    Result& result,
    std::size_t broadcastGlobalLimit) {
    const std::size_t aggregatePendingLimit =
        checkedMultiply(
            config.connections,
            config.hardLimitBytes,
            "aggregate pending limit");
    result.pendingWithinLimit =
        result.pressureOutput.peakPendingBytes <=
            aggregatePendingLimit &&
        result.recoveryOutput.pendingBytes <=
            config.recoveryThresholdBytes;
    result.broadcastWithinLimit =
        result.pressureBroadcast.peakBytes <=
            broadcastGlobalLimit &&
        result.pressureBroadcast.bytes <=
            broadcastGlobalLimit &&
        result.recoveryBroadcast.tasks == 0 &&
        result.recoveryBroadcast.bytes == 0;

    const std::size_t expectedEndpoints =
        checkedMultiply(
            config.connections,
            config.messages,
            "expected endpoint attempts");
    result.terminalAccounted =
        result.terminal.complete &&
        result.terminal.acceptedEndpoints +
                result.terminal.droppedEndpoints ==
            expectedEndpoints;
    result.clientDeliveryAccounted =
        result.clientReceivedBytes ==
        checkedMultiply(
            result.terminal.acceptedEndpoints,
            config.payloadBytes,
            "accepted client bytes");

    const std::size_t endpointOverloaded =
        result.terminal.reasonCounts[
            gamenet::broadcast::reasonIndex(
                gamenet::broadcast::BroadcastReason::
                    EndpointOverloaded)];
    std::size_t otherReasons = 0;
    for (std::size_t index = 0;
         index < result.terminal.reasonCounts.size();
         ++index) {
        if (index !=
            gamenet::broadcast::reasonIndex(
                gamenet::broadcast::BroadcastReason::
                    EndpointOverloaded)) {
            otherReasons += result.terminal.reasonCounts[index];
        }
    }
    result.rejectionAttributed =
        otherReasons == 0 &&
        endpointOverloaded == result.rejections.total();
    result.overloadObserved = endpointOverloaded > 0;
    result.recoveryStable =
        result.recoveryOutput.pendingBytes <=
            config.recoveryThresholdBytes &&
        result.recoveryBroadcast.tasks == 0 &&
        result.recoveryBroadcast.bytes == 0 &&
        result.recoveryElapsedMs >=
            static_cast<double>(
                config.recoveryStable.count());

    const gamenet::net::BufferRetentionOptions bufferOptions;
    const std::size_t bufferRecoveryLimit =
        checkedMultiply(
            checkedMultiply(
                config.connections,
                2,
                "connection buffer count"),
            bufferOptions.maxRetainedCapacityBytes,
            "buffer recovery retention limit");
    result.recoveryRetainedWithinTarget =
        result.recoveryRetention.inputBufferBytes +
                result.recoveryRetention.outputBufferBytes <=
            bufferRecoveryLimit &&
        result.recoveryRetention.transportReadStorageBytes <=
            checkedMultiply(
                config.connections,
                result.fixedRecovery
                    .connectionLocalReadChunkLimitBytes,
                "connection read-storage limit");

    const auto fixedCoherent =
        [](const auto& snapshot) {
            return snapshot.sharedReadPoolBytes == 0 &&
                snapshot.sharedReadSlabBytes == 0 &&
                snapshot.totalRetainedBytes ==
                    snapshot.acceptExFixedPoolBytes +
                        snapshot.iocpCompletionBatchBytes +
                        snapshot.connectionLocalReadBytes &&
                snapshot.totalRetainedBytes <=
                    snapshot.peakTotalRetainedBytes;
        };
    result.fixedStorageCoherent =
        fixedCoherent(result.fixedBaseline) &&
        fixedCoherent(result.fixedPressure) &&
        fixedCoherent(result.fixedRecovery);
    result.teardownReleased =
        result.fixedAfterTeardown.totalRetainedBytes == 0 &&
        result.fixedAfterTeardown.sharedReadPoolBytes == 0 &&
        result.fixedAfterTeardown.sharedReadSlabBytes == 0 &&
        result.fixedAfterTeardown.acceptExFixedPoolBytes == 0 &&
        result.fixedAfterTeardown.iocpCompletionBatchBytes == 0 &&
        result.fixedAfterTeardown.connectionLocalReadBytes == 0;

    if (isMixedProfile(config)) {
        const std::uint64_t expectedAttempts =
            static_cast<std::uint64_t>(
                config.probeTargetPerSecond) *
            static_cast<std::uint64_t>(
                config.probeDuration.count()) /
            1000U;
        const std::uint64_t expectedBatches =
            (expectedAttempts + config.probeBatchSize - 1U) /
            config.probeBatchSize;
        const auto postConnectFailures =
            result.healthyProbe.sendFailures +
            result.healthyProbe.receiveFailures +
            result.healthyProbe.payloadMismatches;
        result.healthyProbeAccounted =
            result.healthyProbe.attempted == expectedAttempts &&
            result.healthyProbe.batches == expectedBatches &&
            result.healthyProbe.attempted ==
                result.healthyProbe.probeSucceeded +
                    result.healthyProbe.totalFailures() &&
            result.healthyProbe.clientConnected ==
                result.healthyProbe.probeSucceeded +
                    postConnectFailures &&
            result.healthyProbe.serverAccepted ==
                result.healthyProbe.clientConnected;
        result.healthyProbeZeroFailures =
            result.healthyProbe.totalFailures() == 0;
        result.healthyProbeClosed =
            result.healthyProbe.serverClosed ==
                result.healthyProbe.serverAccepted;
        result.healthyProbePaced =
            result.healthyProbe.elapsedMs > 0.0 &&
            result.healthyProbe.elapsedMs + 1.0 >=
                static_cast<double>(
                    config.probeDuration.count()) &&
            (std::max)({
                result.healthyProbe.connectP99Us,
                result.healthyProbe.probeP99Us,
                result.healthyProbe.scheduleLagP99Us,
            }) <= result.healthyProbe.elapsedMs * 1000.0 + 1.0;
        result.recoveryReaderPoolAccounted =
            result.recoveryReaders.workers ==
                (std::min)(
                    config.readerConcurrency,
                    config.connections) &&
            result.recoveryReaders.assignedSockets ==
                config.connections &&
            result.recoveryReaders.closedSockets ==
                config.connections;
    }
}

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
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

std::string_view reasonName(
    gamenet::broadcast::BroadcastReason reason) noexcept {
    using gamenet::broadcast::BroadcastReason;
    switch (reason) {
    case BroadcastReason::None: return "none";
    case BroadcastReason::OfflineSession: return "offline_session";
    case BroadcastReason::DuplicateEndpoint: return "duplicate_endpoint";
    case BroadcastReason::FanoutHardLimit: return "fanout_hard_limit";
    case BroadcastReason::ByteHardLimit: return "byte_hard_limit";
    case BroadcastReason::LowPrioritySoftLimit: return "low_priority_soft_limit";
    case BroadcastReason::DispatchTaskByteLimit: return "dispatch_task_byte_limit";
    case BroadcastReason::EndpointClosed: return "endpoint_closed";
    case BroadcastReason::EndpointOverloaded: return "endpoint_overloaded";
    case BroadcastReason::OwnerUnavailable: return "owner_unavailable";
    case BroadcastReason::OwnerShutdown: return "owner_shutdown";
    case BroadcastReason::DispatchQueueFull: return "dispatch_queue_full";
    case BroadcastReason::OwnerOutstandingTaskLimit:
        return "owner_outstanding_task_limit";
    case BroadcastReason::OwnerOutstandingByteLimit:
        return "owner_outstanding_byte_limit";
    case BroadcastReason::GlobalOutstandingByteLimit:
        return "global_outstanding_byte_limit";
    case BroadcastReason::InvalidPlan: return "invalid_plan";
    case BroadcastReason::SendRejected: return "send_rejected";
    case BroadcastReason::Count: return "count";
    }
    return "unknown";
}

void printFixedStorage(
    std::ostream& output,
    const gamenet::net::NetworkFixedStorageRetentionSnapshot&
        snapshot,
    std::string_view indent) {
    output
        << indent << "\"shared_read_pool_bytes\": "
        << snapshot.sharedReadPoolBytes << ",\n"
        << indent << "\"shared_read_slab_bytes\": "
        << snapshot.sharedReadSlabBytes << ",\n"
        << indent << "\"accept_ex_fixed_pool_bytes\": "
        << snapshot.acceptExFixedPoolBytes << ",\n"
        << indent << "\"iocp_completion_batch_bytes\": "
        << snapshot.iocpCompletionBatchBytes << ",\n"
        << indent << "\"connection_local_read_bytes\": "
        << snapshot.connectionLocalReadBytes << ",\n"
        << indent << "\"total_retained_bytes\": "
        << snapshot.totalRetainedBytes << ",\n"
        << indent << "\"peak_total_retained_bytes\": "
        << snapshot.peakTotalRetainedBytes << ",\n"
        << indent << "\"accept_ex_slot_limit_per_acceptor\": "
        << snapshot.acceptExSlotLimitPerAcceptor << ",\n"
        << indent << "\"iocp_completion_batch_entries_per_loop\": "
        << snapshot.iocpCompletionBatchEntriesPerLoop << ",\n"
        << indent << "\"connection_local_read_chunk_limit_bytes\": "
        << snapshot.connectionLocalReadChunkLimitBytes << "\n";
}

void printRetention(
    std::ostream& output,
    const AggregateRetentionSnapshot& snapshot,
    std::string_view indent) {
    output
        << indent << "\"input_buffer_bytes\": "
        << snapshot.inputBufferBytes << ",\n"
        << indent << "\"output_buffer_bytes\": "
        << snapshot.outputBufferBytes << ",\n"
        << indent << "\"transport_read_storage_bytes\": "
        << snapshot.transportReadStorageBytes << ",\n"
        << indent << "\"total_connection_bytes\": "
        << snapshot.totalConnectionBytes << ",\n"
        << indent << "\"peak_input_buffer_bytes\": "
        << snapshot.peakInputBufferBytes << ",\n"
        << indent << "\"peak_output_buffer_bytes\": "
        << snapshot.peakOutputBufferBytes << ",\n"
        << indent << "\"peak_transport_read_storage_bytes\": "
        << snapshot.peakTransportReadStorageBytes << ",\n"
        << indent << "\"input_trim_count\": "
        << snapshot.inputTrimCount << ",\n"
        << indent << "\"output_trim_count\": "
        << snapshot.outputTrimCount << "\n";
}

void printDocument(
    const Config& config,
    const Result& result,
    std::size_t broadcastGlobalLimit,
    std::string_view error) {
    const std::size_t aggregatePendingLimit =
        config.connections * config.hardLimitBytes;
    const auto schema = isMixedProfile(config)
        ? "gamenet.capacity_profile.v3"
        : "gamenet.capacity_profile.v1";
    std::cout
        << std::fixed << std::setprecision(6)
        << "{\n"
        << "  \"schema\": \"" << schema << "\",\n"
        << "  \"status\": \""
        << (error.empty() && result.passed() ? "ok" : "error")
        << "\",\n"
        << "  \"error\": ";
    if (error.empty()) {
        std::cout << "null";
    } else {
        std::cout << '"' << jsonEscape(error) << '"';
    }
    std::cout
        << ",\n"
        << "  \"scenario\": \"" << config.scenario << "\",\n"
        << "  \"platform\": \"" << platformName() << "\",\n"
        << "  \"backend\": \"" << backendName() << "\",\n"
        << "  \"build_type\": \""
        << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
        << "  \"parameters\": {\n"
        << "    \"connections\": " << config.connections << ",\n"
        << "    \"threads\": " << config.threads << ",\n"
        << "    \"messages\": " << config.messages << ",\n"
        << "    \"payload_bytes\": " << config.payloadBytes << ",\n"
        << "    \"server_send_buffer_bytes\": "
        << config.serverSendBufferBytes << ",\n"
        << "    \"pressure_settle_ms\": "
        << config.pressureSettle.count() << ",\n"
        << "    \"recovery_stable_ms\": "
        << config.recoveryStable.count() << ",\n"
        << "    \"timeout_ms\": " << config.timeout.count() << ",\n"
        << "    \"iocp_accept_depth\": "
        << config.iocpAcceptDepth;
    if (isMixedProfile(config)) {
        std::cout
            << ",\n"
            << "    \"probe_target_per_second\": "
            << config.probeTargetPerSecond << ",\n"
            << "    \"probe_duration_ms\": "
            << config.probeDuration.count() << ",\n"
            << "    \"probe_batch_size\": "
            << config.probeBatchSize << ",\n"
            << "    \"probe_concurrency\": "
            << config.probeConcurrency << ",\n"
            << "    \"probe_payload_bytes\": "
            << config.probePayloadBytes << ",\n"
            << "    \"probe_connect_timeout_ms\": "
            << config.probeConnectTimeout.count() << ",\n"
            << "    \"reader_concurrency_limit\": "
            << config.readerConcurrency;
    }
    std::cout
        << "\n"
        << "  },\n"
        << "  \"limits\": {\n"
        << "    \"connection_low_water_bytes\": "
        << config.lowWaterBytes << ",\n"
        << "    \"connection_high_water_bytes\": "
        << config.highWaterBytes << ",\n"
        << "    \"connection_hard_limit_bytes\": "
        << config.hardLimitBytes << ",\n"
        << "    \"aggregate_pending_hard_limit_bytes\": "
        << aggregatePendingLimit << ",\n"
        << "    \"broadcast_global_outstanding_limit_bytes\": "
        << broadcastGlobalLimit << ",\n"
        << "    \"recovery_pending_threshold_bytes\": "
        << config.recoveryThresholdBytes << ",\n"
        << "    \"buffer_max_retained_capacity_bytes\": "
        << gamenet::net::BufferRetentionOptions{}
               .maxRetainedCapacityBytes
        << "\n"
        << "  },\n"
        << "  \"terminal\": {\n"
        << "    \"scheduled_endpoints\": "
        << result.terminal.scheduledEndpoints << ",\n"
        << "    \"accepted_endpoints\": "
        << result.terminal.acceptedEndpoints << ",\n"
        << "    \"dropped_endpoints\": "
        << result.terminal.droppedEndpoints << ",\n"
        << "    \"complete\": "
        << (result.terminal.complete ? "true" : "false")
        << ",\n"
        << "    \"reasons\": {\n";
    for (std::size_t index = 0;
         index < gamenet::broadcast::kBroadcastReasonCount;
         ++index) {
        const auto reason =
            static_cast<gamenet::broadcast::BroadcastReason>(
                index);
        std::cout
            << "      \"" << reasonName(reason) << "\": "
            << result.terminal.reasonCounts[index]
            << (index + 1 ==
                        gamenet::broadcast::kBroadcastReasonCount
                    ? "\n"
                    : ",\n");
    }
    std::cout
        << "    },\n"
        << "    \"tcp_rejections\": {\n"
        << "      \"connection\": " << result.rejections.connection
        << ",\n"
        << "      \"loop\": " << result.rejections.loop << ",\n"
        << "      \"server\": " << result.rejections.server
        << ",\n"
        << "      \"global\": " << result.rejections.global
        << ",\n"
        << "      \"total\": " << result.rejections.total()
        << "\n"
        << "    }\n"
        << "  },\n"
        << "  \"pressure\": {\n"
        << "    \"elapsed_ms\": " << result.pressureElapsedMs << ",\n"
        << "    \"pending_current_bytes\": "
        << result.pressureOutput.pendingBytes << ",\n"
        << "    \"pending_peak_bytes\": "
        << result.pressureOutput.peakPendingBytes << ",\n"
        << "    \"overloaded_connections\": "
        << result.pressureOutput.overloadedConnections << ",\n"
        << "    \"broadcast_outstanding_tasks\": "
        << result.pressureBroadcast.tasks << ",\n"
        << "    \"broadcast_outstanding_bytes\": "
        << result.pressureBroadcast.bytes << ",\n"
        << "    \"broadcast_peak_tasks\": "
        << result.pressureBroadcast.peakTasks << ",\n"
        << "    \"broadcast_peak_bytes\": "
        << result.pressureBroadcast.peakBytes << ",\n"
        << "    \"working_set_bytes\": "
        << result.workingSetPressureBytes << ",\n"
        << "    \"connection_retention\": {\n";
    printRetention(
        std::cout,
        result.pressureRetention,
        "      ");
    std::cout
        << "    },\n"
        << "    \"fixed_storage\": {\n";
    printFixedStorage(
        std::cout,
        result.fixedPressure,
        "      ");
    std::cout
        << "    }\n"
        << "  },\n"
        << "  \"recovery\": {\n"
        << "    \"elapsed_ms\": " << result.recoveryElapsedMs << ",\n"
        << "    \"stable_window_ms\": "
        << config.recoveryStable.count() << ",\n"
        << "    \"pending_current_bytes\": "
        << result.recoveryOutput.pendingBytes << ",\n"
        << "    \"pending_peak_bytes\": "
        << result.recoveryOutput.peakPendingBytes << ",\n"
        << "    \"broadcast_outstanding_tasks\": "
        << result.recoveryBroadcast.tasks << ",\n"
        << "    \"broadcast_outstanding_bytes\": "
        << result.recoveryBroadcast.bytes << ",\n"
        << "    \"working_set_bytes\": "
        << result.workingSetRecoveryBytes << ",\n"
        << "    \"working_set_delta_from_baseline_bytes\": "
        << result.workingSetRecoveryDeltaBytes << ",\n"
        << "    \"connection_retention\": {\n";
    printRetention(
        std::cout,
        result.recoveryRetention,
        "      ");
    std::cout
        << "    },\n"
        << "    \"fixed_storage\": {\n";
    printFixedStorage(
        std::cout,
        result.fixedRecovery,
        "      ");
    std::cout
        << "    }\n"
        << "  }";
    if (isMixedProfile(config)) {
        std::cout
            << ",\n"
            << "  \"healthy_churn\": {\n"
            << "    \"attempted\": "
            << result.healthyProbe.attempted << ",\n"
            << "    \"client_connected\": "
            << result.healthyProbe.clientConnected << ",\n"
            << "    \"server_accepted\": "
            << result.healthyProbe.serverAccepted << ",\n"
            << "    \"probe_succeeded\": "
            << result.healthyProbe.probeSucceeded << ",\n"
            << "    \"server_closed\": "
            << result.healthyProbe.serverClosed << ",\n"
            << "    \"batches\": "
            << result.healthyProbe.batches << ",\n"
            << "    \"elapsed_ms\": "
            << result.healthyProbe.elapsedMs << ",\n"
            << "    \"attempts_per_second\": "
            << result.healthyProbe.attemptsPerSecond << ",\n"
            << "    \"connect_p99_us\": "
            << result.healthyProbe.connectP99Us << ",\n"
            << "    \"probe_p99_us\": "
            << result.healthyProbe.probeP99Us << ",\n"
            << "    \"schedule_lag_p99_us\": "
            << result.healthyProbe.scheduleLagP99Us << ",\n"
            << "    \"failures\": {\n"
            << "      \"connect\": "
            << result.healthyProbe.connectFailures << ",\n"
            << "      \"send\": "
            << result.healthyProbe.sendFailures << ",\n"
            << "      \"receive\": "
            << result.healthyProbe.receiveFailures << ",\n"
            << "      \"payload_mismatch\": "
            << result.healthyProbe.payloadMismatches << ",\n"
            << "      \"total\": "
            << result.healthyProbe.totalFailures() << "\n"
            << "    }\n"
            << "  }";
        std::cout
            << ",\n"
            << "  \"recovery_readers\": {\n"
            << "    \"workers\": "
            << result.recoveryReaders.workers << ",\n"
            << "    \"assigned_sockets\": "
            << result.recoveryReaders.assignedSockets << ",\n"
            << "    \"closed_sockets\": "
            << result.recoveryReaders.closedSockets << "\n"
            << "  }";
    }
    std::cout
        << ",\n"
        << "  \"process\": {\n"
        << "    \"working_set_before_bytes\": "
        << result.workingSetBeforeBytes << ",\n"
        << "    \"working_set_after_bytes\": "
        << result.workingSetAfterBytes << ",\n"
        << "    \"working_set_peak_bytes\": "
        << result.workingSetPeakBytes << ",\n"
        << "    \"client_received_bytes\": "
        << result.clientReceivedBytes << ",\n"
        << "    \"fixed_storage_baseline\": {\n";
    printFixedStorage(
        std::cout,
        result.fixedBaseline,
        "      ");
    std::cout
        << "    },\n"
        << "    \"fixed_storage_after_teardown\": {\n";
    printFixedStorage(
        std::cout,
        result.fixedAfterTeardown,
        "      ");
    std::cout
        << "    }\n"
        << "  },\n"
        << "  \"checks\": {\n"
        << "    \"pending_within_limit\": "
        << (result.pendingWithinLimit ? "true" : "false") << ",\n"
        << "    \"broadcast_within_limit\": "
        << (result.broadcastWithinLimit ? "true" : "false") << ",\n"
        << "    \"terminal_accounted\": "
        << (result.terminalAccounted ? "true" : "false") << ",\n"
        << "    \"client_delivery_accounted\": "
        << (result.clientDeliveryAccounted ? "true" : "false") << ",\n"
        << "    \"rejection_attributed\": "
        << (result.rejectionAttributed ? "true" : "false") << ",\n"
        << "    \"overload_observed\": "
        << (result.overloadObserved ? "true" : "false") << ",\n"
        << "    \"recovery_stable\": "
        << (result.recoveryStable ? "true" : "false") << ",\n"
        << "    \"recovery_retained_within_target\": "
        << (result.recoveryRetainedWithinTarget ? "true" : "false")
        << ",\n"
        << "    \"fixed_storage_coherent\": "
        << (result.fixedStorageCoherent ? "true" : "false") << ",\n"
        << "    \"teardown_released\": "
        << (result.teardownReleased ? "true" : "false");
    if (isMixedProfile(config)) {
        std::cout
            << ",\n"
            << "    \"healthy_probe_accounted\": "
            << (result.healthyProbeAccounted ? "true" : "false")
            << ",\n"
            << "    \"healthy_probe_zero_failures\": "
            << (result.healthyProbeZeroFailures ? "true" : "false")
            << ",\n"
            << "    \"healthy_probe_closed\": "
            << (result.healthyProbeClosed ? "true" : "false")
            << ",\n"
            << "    \"healthy_probe_paced\": "
            << (result.healthyProbePaced ? "true" : "false")
            << ",\n"
            << "    \"recovery_reader_pool_accounted\": "
            << (result.recoveryReaderPoolAccounted
                    ? "true"
                    : "false");
    }
    std::cout
        << ",\n"
        << "    \"passed\": "
        << (result.passed() ? "true" : "false") << "\n"
        << "  }\n"
        << "}\n";
    std::cout.flush();
    if (!std::cout) {
        throw std::runtime_error(
            "failed to flush capacity profile JSON");
    }
}

int run(const Config& config) {
    gamenet::base::Logger::setLogLevel(
        gamenet::base::Logger::FATAL);
    Result result;
    SharedState state;
    std::atomic<bool> stopIssued{false};
    const std::size_t logicalBytes =
        checkedMultiply(
            checkedMultiply(
                config.connections,
                config.messages,
                "logical endpoint attempts"),
            config.payloadBytes,
            "logical broadcast bytes");
    const std::size_t broadcastGlobalLimit =
        (std::max)(logicalBytes, config.payloadBytes);

    result.workingSetBeforeBytes = sampleWorkingSetBytes();
    WorkingSetSampler workingSetSampler(
        result.workingSetBeforeBytes);

    {
        gamenet::net::EventLoop loop;
        gamenet::net::TcpServer server(
            &loop,
            gamenet::net::InetAddress(0, true),
            config.scenario + "-profile");
        server.setThreadNum(static_cast<int>(config.threads));
        server.setIocpAcceptDepth(config.iocpAcceptDepth);
        server.setConnectionBackpressureOptions({
            .lowWaterMarkBytes = config.lowWaterBytes,
            .highWaterMarkBytes = config.highWaterBytes,
            .hardLimitBytes = config.hardLimitBytes,
            .maxInputBufferBytes = 2U * 1024U * 1024U,
        });

        const std::size_t outputHeadroomConnections =
            config.connections +
            (isMixedProfile(config)
                 ? config.probeBatchSize
                 : 0);
        const std::size_t loopBudgetLimit =
            checkedMultiply(
                outputHeadroomConnections,
                config.hardLimitBytes,
                "loop output budget");
        const std::size_t serverBudgetLimit =
            checkedMultiply(
                loopBudgetLimit,
                2,
                "server output budget");
        server.setOutputMemoryOptions({
            .loop = {
                .hardLimitBytes = loopBudgetLimit,
                .recoveryThresholdBytes =
                    (std::min)(
                        loopBudgetLimit,
                        checkedMultiply(
                            outputHeadroomConnections,
                            config.lowWaterBytes,
                            "loop recovery budget")),
            },
            .server = {
                .hardLimitBytes = serverBudgetLimit,
                .recoveryThresholdBytes =
                    (std::min)(
                        serverBudgetLimit,
                        checkedMultiply(
                            outputHeadroomConnections,
                            config.lowWaterBytes,
                            "server recovery budget")),
            },
        });

        gamenet::broadcast::BroadcastRouter router(
            &loop,
            {
                .softFanout = config.connections,
                .hardFanout = config.connections,
                .softBytes = checkedMultiply(
                    config.connections,
                    config.payloadBytes,
                    "router soft bytes"),
                .hardBytes = checkedMultiply(
                    config.connections,
                    config.payloadBytes,
                    "router hard bytes"),
            });
        gamenet::broadcast::BroadcastDispatcher dispatcher({
            .maxEndpointsPerTask = config.connections,
            .maxBytesPerTask = checkedMultiply(
                config.connections,
                config.payloadBytes,
                "dispatch task bytes"),
            .maxOutstandingTasksPerOwner =
                checkedMultiply(
                    config.messages,
                    config.threads + 1,
                    "owner outstanding tasks"),
            .maxOutstandingBytesPerOwner =
                broadcastGlobalLimit,
            .maxGlobalOutstandingBytes =
                broadcastGlobalLimit,
            .lowPriorityOutstandingBytes =
                broadcastGlobalLimit,
        });

        std::vector<ConnectionHandle> baseHandles;
        baseHandles.reserve(config.connections);
        std::atomic<std::uint64_t> nextTransportId{1};

        server.setConnectionCallback(
            [&](const gamenet::net::TcpConnectionPtr& connection) {
                if (!connection->connected()) {
                    state.markDisconnected(connection.get());
                    return;
                }
                try {
                    if (config.serverSendBufferBytes != 0) {
                        connection->setSendBufferSize(
                            config.serverSendBufferBytes);
                    }
                    if (state.classifyConnected(
                            connection.get(),
                            config.connections)) {
                        return;
                    }
                    auto endpoint = std::make_shared<
                        gamenet::transport::TcpTransportEndpoint>(
                        gamenet::transport::TransportSessionId{
                            nextTransportId.fetch_add(
                                1,
                                std::memory_order_relaxed)},
                        connection);
                    loop.queueInLoop(
                        [&, connection, endpoint = std::move(endpoint)] {
                            if (baseHandles.size() >=
                                config.connections) {
                                state.fail(
                                    "received more connections than configured");
                                server.stop();
                                return;
                            }
                            baseHandles.push_back({
                                .connection = connection,
                                .endpoint = endpoint,
                            });
                            if (baseHandles.size() !=
                                config.connections) {
                                return;
                            }

                            try {
                                std::vector<
                                    gamenet::broadcast::BroadcastTarget>
                                    targets;
                                targets.reserve(baseHandles.size());
                                for (const auto& handle : baseHandles) {
                                    targets.emplace_back(handle.endpoint);
                                }

                                std::vector<std::shared_ptr<
                                    gamenet::broadcast::DispatchProgress>>
                                    progress;
                                progress.reserve(config.messages);
                                for (std::size_t sequence = 0;
                                     sequence < config.messages;
                                     ++sequence) {
                                    auto payload =
                                        std::make_shared<const std::string>(
                                            makePayload(
                                                sequence,
                                                config.payloadBytes));
                                    auto plan =
                                        router.route(payload, targets);
                                    auto summary =
                                        dispatcher.dispatch(
                                            std::move(plan));
                                    progress.push_back(
                                        std::move(summary.progress));
                                }
                                state.publish(baseHandles, std::move(progress));
                            } catch (const std::exception& error) {
                                state.fail(error.what());
                                server.stop();
                            }
                        });
                } catch (const std::exception& error) {
                    state.fail(error.what());
                    server.stop();
                }
            });
        if (isMixedProfile(config)) {
            server.setMessageCallback(
                [&](const gamenet::net::TcpConnectionPtr& connection,
                    gamenet::net::Buffer* buffer) {
                    const std::size_t readable =
                        buffer->readableBytes();
                    const auto sendResult =
                        connection->trySend(
                            buffer->peek(),
                            readable);
                    buffer->retrieve(readable);
                    if (sendResult !=
                        gamenet::net::TcpSendResult::Accepted) {
                        connection->forceClose();
                    }
                });
        }

        server.start();
        result.fixedBaseline =
            gamenet::net::
                networkFixedStorageRetentionSnapshot();
        const auto address = server.listenAddress();

        std::thread driver([&] {
            std::vector<ClientSocket> clients;
            std::vector<std::thread> readers;
            std::thread probeThread;
            std::atomic<std::uint64_t> receivedBytes{0};
            std::atomic<std::uint64_t> readerClosedSockets{0};
            std::atomic<bool> readerFailed{false};
            std::mutex readerFailureMutex;
            std::exception_ptr readerFailure;
            gamenet::net::TcpServerStopFuture gracefulStop;
            const auto joinProbe = [&] {
                if (probeThread.joinable()) {
                    probeThread.join();
                }
            };
            try {
                clients.reserve(config.connections);
                for (std::size_t index = 0;
                     index < config.connections;
                     ++index) {
                    clients.push_back(
                        ClientSocket::connectTo(
                            address,
                            config.timeout));
                }

                std::vector<ConnectionHandle> handles;
                std::vector<std::shared_ptr<
                    gamenet::broadcast::DispatchProgress>>
                    progress;
                if (!state.waitForPublished(
                        config.timeout,
                        &handles,
                        &progress)) {
                    const auto failure = state.failure();
                    throw std::runtime_error(
                        failure.empty()
                            ? "timed out waiting for real TCP endpoints"
                            : failure);
                }
                if (isMixedProfile(config)) {
                    probeThread = std::thread([&] {
                        try {
                            runHealthyProbes(
                                config,
                                address,
                                state,
                                result);
                        } catch (const std::exception& error) {
                            state.fail(error.what());
                        }
                    });
                }

                const auto pressureStarted = Clock::now();
                if (!waitForTerminalProgress(
                        progress,
                        config.timeout,
                        &result.terminal)) {
                    throw std::runtime_error(
                        "timed out waiting for broadcast terminal accounting");
                }
                std::this_thread::sleep_for(
                    config.pressureSettle);

                result.pressureElapsedMs =
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - pressureStarted)
                        .count();
                result.pressureOutput =
                    aggregateOutput(handles);
                result.pressureRetention =
                    aggregateRetention(handles, config.timeout);
                result.pressureBroadcast =
                    dispatcher.outstanding();
                result.fixedPressure =
                    gamenet::net::
                        networkFixedStorageRetentionSnapshot();
                result.workingSetPressureBytes =
                    sampleWorkingSetBytes();

                if (isMixedProfile(config)) {
                    for (auto& client : clients) {
                        client.makeNonblocking();
                    }
                    const std::size_t readerWorkerCount =
                        (std::min)(
                            config.readerConcurrency,
                            clients.size());
                    result.recoveryReaders.workers =
                        readerWorkerCount;
                    result.recoveryReaders.assignedSockets =
                        clients.size();
                    readers.reserve(readerWorkerCount);
                    for (std::size_t workerIndex = 0;
                         workerIndex < readerWorkerCount;
                         ++workerIndex) {
                        readers.emplace_back(
                            [&, workerIndex, readerWorkerCount] {
                                try {
                                    std::vector<unsigned char>
                                        closed;
                                    for (std::size_t index =
                                             workerIndex;
                                         index < clients.size();
                                         index +=
                                             readerWorkerCount) {
                                        closed.push_back(0);
                                    }
                                    std::size_t remaining =
                                        closed.size();
                                    std::array<
                                        char,
                                        64U * 1024U>
                                        buffer{};
                                    std::optional<
                                        Clock::time_point>
                                        shutdownDeadline;
                                    while (remaining != 0) {
                                        const bool shutdownExpected =
                                            stopIssued.load(
                                                std::memory_order_acquire);
                                        if (shutdownExpected &&
                                            !shutdownDeadline) {
                                            shutdownDeadline =
                                                Clock::now() +
                                                config.timeout;
                                        }
                                        bool progressed = false;
                                        std::size_t localIndex = 0;
                                        for (std::size_t index =
                                                 workerIndex;
                                             index < clients.size();
                                             index +=
                                                 readerWorkerCount,
                                             ++localIndex) {
                                            if (closed[localIndex] != 0) {
                                                continue;
                                            }
                                            const auto drained =
                                                clients[index]
                                                    .drainAvailable(
                                                        buffer,
                                                        shutdownExpected);
                                            if (drained.bytes != 0) {
                                                receivedBytes.fetch_add(
                                                    drained.bytes,
                                                    std::memory_order_relaxed);
                                                progressed = true;
                                            }
                                            if (drained.closed) {
                                                closed[localIndex] = 1;
                                                --remaining;
                                                readerClosedSockets.fetch_add(
                                                    1,
                                                    std::memory_order_release);
                                                progressed = true;
                                            }
                                        }
                                        if (remaining != 0 &&
                                            shutdownDeadline &&
                                            Clock::now() >=
                                                *shutdownDeadline) {
                                            throw std::runtime_error(
                                                "bounded recovery reader "
                                                "shutdown timed out");
                                        }
                                        if (!progressed) {
                                            std::this_thread::sleep_for(
                                                1ms);
                                        }
                                    }
                                } catch (...) {
                                    readerFailed.store(
                                        true,
                                        std::memory_order_release);
                                    std::lock_guard lock(
                                        readerFailureMutex);
                                    if (!readerFailure) {
                                        readerFailure =
                                            std::current_exception();
                                    }
                                }
                            });
                    }
                } else {
                    readers.reserve(clients.size());
                    for (std::size_t index = 0;
                         index < clients.size();
                         ++index) {
                        readers.emplace_back([&, index] {
                            try {
                                receivedBytes.fetch_add(
                                    clients[index]
                                        .drainUntilClosed(
                                            stopIssued),
                                    std::memory_order_relaxed);
                            } catch (...) {
                                std::lock_guard lock(
                                    readerFailureMutex);
                                if (!readerFailure) {
                                    readerFailure =
                                        std::current_exception();
                                }
                            }
                        });
                    }
                }

                const auto recoveryStarted = Clock::now();
                const auto recoveryDeadline =
                    recoveryStarted + config.timeout;
                std::optional<Clock::time_point> stableSince;
                bool recovered = false;
                while (Clock::now() < recoveryDeadline) {
                    if (readerFailed.load(
                            std::memory_order_acquire)) {
                        throw std::runtime_error(
                            "bounded recovery reader failed");
                    }
                    const auto output =
                        aggregateOutput(handles);
                    const auto broadcast =
                        dispatcher.outstanding();
                    if (output.pendingBytes <=
                            config.recoveryThresholdBytes &&
                        broadcast.tasks == 0 &&
                        broadcast.bytes == 0) {
                        if (!stableSince) {
                            stableSince = Clock::now();
                        }
                        if (Clock::now() - *stableSince >=
                            config.recoveryStable) {
                            recovered = true;
                            break;
                        }
                    } else {
                        stableSince.reset();
                    }
                    std::this_thread::sleep_for(5ms);
                }
                if (!recovered) {
                    throw std::runtime_error(
                        "pending output did not enter the stable recovery window");
                }

                result.recoveryElapsedMs =
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - recoveryStarted)
                        .count();
                result.recoveryOutput =
                    aggregateOutput(handles);
                result.recoveryRetention =
                    aggregateRetention(handles, config.timeout);
                result.recoveryBroadcast =
                    dispatcher.outstanding();
                result.fixedRecovery =
                    gamenet::net::
                        networkFixedStorageRetentionSnapshot();
                result.workingSetRecoveryBytes =
                    sampleWorkingSetBytes();
                result.workingSetRecoveryDeltaBytes =
                    signedDelta(
                        result.workingSetRecoveryBytes,
                        result.workingSetBeforeBytes);
                result.rejections =
                    collectRejections(
                        result.recoveryOutput,
                        server.outputMemoryStats());

                joinProbe();
                const auto probeFailure = state.failure();
                if (!probeFailure.empty()) {
                    throw std::runtime_error(probeFailure);
                }
                stopIssued.store(true, std::memory_order_release);
                gracefulStop = server.stopGracefully({
                    .drainTimeout = config.timeout,
                });
            } catch (const std::exception& error) {
                state.fail(error.what());
                stopIssued.store(true, std::memory_order_release);
                server.stop();
                joinProbe();
            }
            for (auto& reader : readers) {
                if (reader.joinable()) {
                    reader.join();
                }
            }
            if (isMixedProfile(config)) {
                result.recoveryReaders.closedSockets =
                    readerClosedSockets.load(
                        std::memory_order_acquire);
            }
            if (readerFailure) {
                try {
                    std::rethrow_exception(readerFailure);
                } catch (const std::exception& error) {
                    state.fail(error.what());
                }
            }
            result.clientReceivedBytes =
                receivedBytes.load(std::memory_order_relaxed);
            clients.clear();
            if (gracefulStop.valid() &&
                gracefulStop.wait_for(config.timeout) !=
                    std::future_status::ready) {
                state.fail(
                    "graceful capacity-profile teardown timed out");
                server.stop();
            }
            state.markDriverDone();
        });

        bool quitScheduled = false;
        loop.runEvery(10ms, [&] {
            if (quitScheduled || !state.driverDone()) {
                return;
            }
            if (server.connectionCount() != 0) {
                return;
            }
            quitScheduled = true;
            server.stop();
            loop.runAfter(50ms, [&] { loop.quit(); });
        });
        const auto overallTimeout =
            std::chrono::milliseconds(
                config.timeout.count() * 3 +
                config.pressureSettle.count() +
                config.recoveryStable.count() +
                5000);
        loop.runAfter(overallTimeout, [&] {
            if (quitScheduled) {
                return;
            }
            state.fail("capacity profile overall timeout");
            stopIssued.store(true, std::memory_order_release);
            server.stop();
        });
        loop.loop();
        driver.join();
    }

    result.fixedAfterTeardown =
        gamenet::net::networkFixedStorageRetentionSnapshot();
    result.workingSetAfterBytes = sampleWorkingSetBytes();
    const auto sampledWorkingSetPeak =
        workingSetSampler.stop(result.workingSetAfterBytes);
    result.workingSetPeakBytes = (std::max)({
        sampledWorkingSetPeak,
        result.workingSetBeforeBytes,
        result.workingSetPressureBytes,
        result.workingSetRecoveryBytes,
        result.workingSetAfterBytes,
    });
    finalizeChecks(
        config,
        result,
        broadcastGlobalLimit);

    std::string failure = state.failure();
    if (failure.empty() && !result.passed()) {
        failure =
            "one or more capacity profile invariants failed";
    }
    printDocument(
        config,
        result,
        broadcastGlobalLimit,
        failure);
    return failure.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    Config config;
    try {
        config = parseArgs(argc, argv);
        return run(config);
    } catch (const HelpRequested&) {
        printUsage(std::cout);
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gamenet_capacity_profile: "
            << error.what() << '\n';
        printUsage(std::cerr);
        Result result;
        printDocument(
            config,
            result,
            0,
            error.what());
        return 2;
    }
}
