#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/InetAddress.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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

struct Config {
    std::string scenario{"echo"};
    std::size_t connections{0};
    std::size_t eventLoopThreads{1};
    std::size_t messagesPerConnection{1000};
    std::size_t payloadBytes{256};
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
    std::uint64_t offeredBytes{0};
    std::uint64_t workingSetBefore{0};
    std::uint64_t workingSetAfter{0};
    std::int64_t workingSetDelta{0};
    std::optional<double> throughputMiBPerSecond;
    std::optional<double> p50LatencyUs;
    std::optional<double> p99LatencyUs;
    std::optional<double> approxBytesPerConnection;
    std::uint64_t highWaterCallbacks{0};
};

class SharedState {
public:
    void markConnected() {
        connected_.fetch_add(1, std::memory_order_release);
        cv_.notify_all();
    }

    void markDisconnected() {
        disconnected_.fetch_add(1, std::memory_order_release);
        cv_.notify_all();
    }

    void markHighWater() {
        highWaterCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    bool waitForConnections(std::size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return connected_.load(std::memory_order_acquire) >= expected || !failure_.empty();
        }) && connected_.load(std::memory_order_acquire) >= expected;
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

    std::uint64_t highWaterCallbacks() const noexcept {
        return highWaterCallbacks_.load(std::memory_order_relaxed);
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
    std::atomic<bool> driverDone_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
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
        << "  --scenario echo|connections|slow-client\n"
        << "  --connections N       scenario default: echo=4, connections=256, slow-client=4\n"
        << "  --threads N           TcpServer worker EventLoops, including 0 for the base loop\n"
        << "  --messages N          echo messages per connection (default 1000)\n"
        << "  --payload N           echo payload bytes (default 256)\n"
        << "  --slow-bytes N        bytes offered to each slow client (default 8388608)\n"
        << "  --high-water N        high-water notification threshold (default 65536)\n"
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
            config.connections = parseSize(value, option, 1, 4096);
            config.connectionsProvided = true;
        } else if (option == "--threads") {
            config.eventLoopThreads = parseSize(value, option, 0, 64);
        } else if (option == "--messages") {
            config.messagesPerConnection = parseSize(value, option, 1, 1000000);
        } else if (option == "--payload") {
            config.payloadBytes = parseSize(value, option, 1, 64U * kMebibyte);
        } else if (option == "--slow-bytes") {
            config.slowBytes = parseSize(value, option, 1, 256U * kMebibyte);
        } else if (option == "--high-water") {
            config.highWaterBytes = parseSize(value, option, 1, 256U * kMebibyte);
        } else if (option == "--settle-ms") {
            config.settle = std::chrono::milliseconds(parseSize(value, option, 0, 60000));
        } else if (option == "--timeout-ms") {
            config.timeout = std::chrono::milliseconds(parseSize(value, option, 100, 300000));
        } else {
            usageError(std::string("unknown option: ") + std::string(option));
        }
    }

    if (config.scenario != "echo" && config.scenario != "connections" &&
        config.scenario != "slow-client") {
        usageError("--scenario must be echo, connections, or slow-client");
    }
    if (!config.connectionsProvided) {
        config.connections = config.scenario == "connections" ? 256U : 4U;
    }
    if (config.highWaterBytes > config.slowBytes && config.scenario == "slow-client") {
        usageError("--high-water must not exceed --slow-bytes for slow-client");
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

private:
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
    std::vector<ClientSocket> clients;
    clients.reserve(config.connections);
    for (std::size_t index = 0; index < config.connections; ++index) {
        clients.push_back(ClientSocket::connectTo(address, config.timeout, slowReaders));
    }
    if (!state.waitForConnections(config.connections, config.timeout)) {
        throw std::runtime_error("timed out waiting for TcpServer connection callbacks");
    }
    return clients;
}

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

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) {
        throw std::runtime_error("cannot calculate percentile without samples");
    }
    std::sort(samples.begin(), samples.end());
    const double rank = std::ceil(fraction * static_cast<double>(samples.size()));
    const auto nearestRank = static_cast<std::size_t>(rank < 1.0 ? 1.0 : rank);
    return samples[(std::min)(nearestRank - 1, samples.size() - 1)];
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
        result.p50LatencyUs = percentile(allSamples, 0.50);
        result.p99LatencyUs = percentile(std::move(allSamples), 0.99);
    }
    clients.clear();
}

void runConnections(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const auto started = Clock::now();
    auto clients = connectClients(config, address, state, false);
    std::this_thread::sleep_for(config.settle);
    recordWorkingSet(result, config);
    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    clients.clear();
}

void runSlowClient(
    const Config& config,
    const gamenet::net::InetAddress& address,
    SharedState& state,
    Result& result) {
    const auto started = Clock::now();
    auto clients = connectClients(config, address, state, true);
    std::this_thread::sleep_for(config.settle);
    recordWorkingSet(result, config);
    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    result.offeredBytes = static_cast<std::uint64_t>(config.connections) * config.slowBytes;
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

void printOptional(std::ostream& output, const std::optional<double>& value) {
    if (value) {
        output << *value;
    } else {
        output << "null";
    }
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
    return "single_get_queued_completion_status";
#else
    return "epoll_wait_batch";
#endif
}

void printResult(const Config& config, const Result& result, const SharedState& state) {
    const std::string failure = state.failure();
    std::cout << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"schema\": \"gamenet.core_benchmark.v1\",\n"
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
              << "  \"backpressure_policy\": \"high_water_notification_only\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE << "\",\n"
              << "  \"parameters\": {\n"
              << "    \"connections\": " << config.connections << ",\n"
              << "    \"event_loop_threads\": " << config.eventLoopThreads << ",\n"
              << "    \"messages_per_connection\": " << config.messagesPerConnection << ",\n"
              << "    \"payload_bytes\": " << config.payloadBytes << ",\n"
              << "    \"slow_bytes_per_connection\": " << config.slowBytes << ",\n"
              << "    \"high_water_bytes\": " << config.highWaterBytes << ",\n"
              << "    \"settle_ms\": " << config.settle.count() << ",\n"
              << "    \"timeout_ms\": " << config.timeout.count() << "\n"
              << "  },\n"
              << "  \"measurements\": {\n"
              << "    \"elapsed_seconds\": " << result.elapsedSeconds << ",\n"
              << "    \"round_trips\": " << result.roundTrips << ",\n"
              << "    \"application_bytes\": " << result.applicationBytes << ",\n"
              << "    \"throughput_mib_per_second\": ";
    printOptional(std::cout, result.throughputMiBPerSecond);
    std::cout << ",\n    \"p50_latency_us\": ";
    printOptional(std::cout, result.p50LatencyUs);
    std::cout << ",\n    \"p99_latency_us\": ";
    printOptional(std::cout, result.p99LatencyUs);
    std::cout << ",\n"
              << "    \"working_set_before_bytes\": " << result.workingSetBefore << ",\n"
              << "    \"working_set_after_bytes\": " << result.workingSetAfter << ",\n"
              << "    \"working_set_delta_bytes\": " << result.workingSetDelta << ",\n"
              << "    \"approx_bytes_per_connection\": ";
    printOptional(std::cout, result.approxBytesPerConnection);
    std::cout << ",\n"
              << "    \"offered_bytes\": " << result.offeredBytes << ",\n"
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
    server.setThreadNum(static_cast<int>(config.eventLoopThreads));

    SharedState state;
    Result result;
    const std::string payload(
        config.scenario == "slow-client" ? config.slowBytes : config.payloadBytes,
        'x');

    server.setConnectionCallback([&](const gamenet::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            if (config.scenario == "slow-client") {
                connection->send(payload);
            }
            state.markConnected();
        } else {
            state.markDisconnected();
        }
    });
    if (config.scenario == "echo") {
        server.setMessageCallback(
            [](const gamenet::net::TcpConnectionPtr& connection, gamenet::net::Buffer* buffer) {
                const std::size_t readable = buffer->readableBytes();
                connection->send(buffer->peek(), readable);
                buffer->retrieve(readable);
            });
    }
    if (config.scenario == "slow-client") {
        server.setHighWaterMarkCallback(
            [&](const gamenet::net::TcpConnectionPtr&, std::size_t) { state.markHighWater(); },
            config.highWaterBytes);
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
            } else {
                runSlowClient(config, address, state, result);
            }
        } catch (const std::exception& error) {
            state.fail(error.what());
        }
        state.markDriverDone();
    });

    bool finishing = false;
    loop.runEvery(10ms, [&] {
        if (finishing || !state.driverDone()) {
            return;
        }
        if (state.connected() != state.disconnected()) {
            return;
        }
        if (server.connectionCount() == 0) {
            finishing = true;
            server.stop();
            loop.runAfter(100ms, [&] { loop.quit(); });
        }
    });
    loop.runAfter(config.timeout + config.settle + 5s, [&] {
        if (finishing) {
            return;
        }
        state.fail("benchmark overall timeout");
        server.stop();
    });
    loop.loop();
    driver.join();

    result.highWaterCallbacks = state.highWaterCallbacks();
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
