#include "runtime_profiles/MultiIoShardedHybrid.h"

#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/EventLoopThread.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <psapi.h>
#else
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

struct Config {
    std::size_t clients{4};
    std::size_t messagesPerClient{2000};
    std::size_t payloadBytes{256};
    std::size_t batchMessages{64};
};

std::size_t parsePositive(const char* text, std::string_view name) {
    try {
        const std::string value(text);
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size() || parsed == 0) {
            throw std::invalid_argument("not positive");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string(name) + " must be a positive integer");
    }
}

Config parseConfig(int argc, char* argv[]) {
    if (argc > 4) {
        throw std::invalid_argument(
            "usage: gamenet_multi_io_sharded_hybrid_benchmark "
            "[messages-per-client] [payload-bytes] [clients]");
    }
    Config config;
    if (argc > 1) config.messagesPerClient = parsePositive(argv[1], "messages");
    if (argc > 2) config.payloadBytes = parsePositive(argv[2], "payload");
    if (argc > 3) config.clients = parsePositive(argv[3], "clients");
    if (config.payloadBytes < 32 || config.payloadBytes > 64U * 1024U ||
        config.clients < 2 || config.clients > 256) {
        throw std::invalid_argument(
            "payload or client count exceeds benchmark bound");
    }
    config.batchMessages = std::min(
        config.batchMessages, config.messagesPerClient);
    return config;
}

gamenet::net::SocketFd connectClient(
    const gamenet::net::InetAddress& address) {
    const auto fd =
        gamenet::net::sockets::createNonblockingOrDie(address.family());
    if (gamenet::net::sockets::connect(
            fd, address.getSockAddr(), address.getSockAddrLen()) < 0) {
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isInProgress(error) &&
            !gamenet::net::sockets::isWouldBlock(error)) {
            gamenet::net::sockets::close(fd);
            throw std::runtime_error("benchmark client connect failed");
        }
    }
    return fd;
}

bool writeAll(
    gamenet::net::SocketFd fd,
    std::string_view bytes,
    std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size() &&
           std::chrono::steady_clock::now() < deadline) {
        const auto written = gamenet::net::sockets::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInProgress(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::yield();
    }
    return offset == bytes.size();
}

bool readBatch(
    gamenet::net::SocketFd fd,
    gamenet::protocol::PacketFramer& decoder,
    std::size_t expectedFrames,
    std::chrono::steady_clock::time_point deadline) {
    std::array<char, 64U * 1024U> bytes{};
    std::size_t received = 0;
    while (received < expectedFrames &&
           std::chrono::steady_clock::now() < deadline) {
        const auto count = gamenet::net::sockets::read(
            fd, bytes.data(), bytes.size());
        if (count > 0) {
            auto result = decoder.push(
                std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            while (true) {
                received += result.frames.size();
                if (!result.needsContinuation) break;
                result = decoder.push({});
            }
            continue;
        }
        if (count == 0) return false;
        const auto error = gamenet::net::sockets::lastError();
        if (!gamenet::net::sockets::isWouldBlock(error) &&
            !gamenet::net::sockets::isInterrupted(error)) {
            return false;
        }
        std::this_thread::yield();
    }
    return received == expectedFrames;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
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

gamenet::examples::MultiIoShardedHybridRoute routePayload(
    gamenet::transport::TransportSessionId,
    std::string_view payload) {
    const auto first = payload.find('|');
    const auto second = first == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', first + 1);
    const auto third = second == std::string_view::npos
        ? std::string_view::npos
        : payload.find('|', second + 1);
    if (first != 1 || second == std::string_view::npos ||
        third == std::string_view::npos) {
        throw std::invalid_argument("invalid benchmark route");
    }
    if (payload.front() != 'e' && payload.front() != 'f') {
        throw std::invalid_argument("benchmark lane must be e or f");
    }
    return {
        .key = {
            .kind = gamenet::examples::LogicShardKeyKind::Room,
            .value = std::string(payload.substr(second + 1, third - second - 1)),
        },
        .lane = payload.front() == 'f'
            ? gamenet::examples::HybridDispatchLane::FixedTick
            : gamenet::examples::HybridDispatchLane::EventDriven,
    };
}

std::vector<gamenet::examples::LogicShardKey> keysForCells(
    const gamenet::examples::MultiIoShardedHybrid& profile,
    std::size_t count) {
    std::vector<std::optional<gamenet::examples::LogicShardKey>> selected(count);
    for (std::size_t index = 0; index < 10000; ++index) {
        gamenet::examples::LogicShardKey key{
            .kind = gamenet::examples::LogicShardKeyKind::Room,
            .value = "bench-room-" + std::to_string(index),
        };
        const auto cell = profile.logicShardForKey(key);
        if (!selected[cell]) selected[cell] = std::move(key);
        if (std::all_of(selected.begin(), selected.end(), [](const auto& item) {
                return item.has_value();
            })) {
            std::vector<gamenet::examples::LogicShardKey> result;
            result.reserve(count);
            for (auto& item : selected) result.push_back(std::move(*item));
            return result;
        }
    }
    throw std::runtime_error("failed to select one key per logic cell");
}

std::string benchmarkPayload(
    gamenet::examples::HybridDispatchLane lane,
    const gamenet::examples::LogicShardKey& key,
    std::size_t payloadBytes) {
    std::string payload = lane == gamenet::examples::HybridDispatchLane::FixedTick
        ? "f|r|" : "e|r|";
    payload += key.value;
    payload += '|';
    if (payload.size() > payloadBytes) {
        throw std::invalid_argument("benchmark payload is smaller than route prefix");
    }
    payload.append(payloadBytes - payload.size(), 'p');
    return payload;
}

int run(const Config& config) {
    const auto workingSetBefore = sampleWorkingSetBytes();
    gamenet::net::EventLoopThread firstLogicThread;
    gamenet::net::EventLoopThread secondLogicThread;
    auto* firstLogicLoop = firstLogicThread.startLoop();
    auto* secondLogicLoop = secondLogicThread.startLoop();
    gamenet::examples::MultiIoShardedHybridMetrics metrics;
    double elapsedSeconds = 0.0;
    std::uint64_t shutdownMicros = 0;
    bool clientsSucceeded = true;

    {
        gamenet::net::EventLoop baseLoop;
        gamenet::examples::MultiIoShardedHybridOptions options;
        options.ioThreads = 2;
        options.framing.maxPayloadBytes = 64U * 1024U;
        options.framing.maxBufferedBytes = 2U * (64U * 1024U + 4U);
        options.framing.maxFramesPerPush = 256;
        options.framing.maxFrameBytesPerPush = 4U * 1024U * 1024U;
        options.maxCommandsPerCell = 65536;
        options.maxQueuedBytesPerCell = 64U * 1024U * 1024U;
        options.maxPayloadBytes = 64U * 1024U;
        options.maxShardKeyBytes = 256;
        options.maxCommandsPerEventDrain = 1024;
        options.maxCommandsPerTick = 1024;
        options.tickInterval = 1ms;
        options.admission.maxConnections = config.clients;
        options.maxRouterWallTime = 100ms;
        options.maxHandlerWallTime = 100ms;
        options.maxTurnWallTime = 10ms;

        auto profile =
            std::make_unique<gamenet::examples::MultiIoShardedHybrid>(
                &baseLoop,
                std::vector<gamenet::net::EventLoop*>{
                    firstLogicLoop,
                    secondLogicLoop,
                },
                gamenet::net::InetAddress(0, true),
                routePayload,
                [](const gamenet::examples::MultiIoShardedHybridContext&,
                   auto,
                   std::string_view payload) {
                    return gamenet::examples::MultiIoShardedHybridHandlerResult{
                        .reply = std::string(payload),
                    };
                },
                options);
        const auto keys = keysForCells(*profile, 2);
        profile->start();

        gamenet::protocol::PacketFramer encoder(options.framing);
        struct EncodedPair {
            std::string event;
            std::string fixed;
        };
        std::array<EncodedPair, 2> encodedByCell;
        for (std::size_t cell = 0; cell < encodedByCell.size(); ++cell) {
            const auto eventPayload = benchmarkPayload(
                gamenet::examples::HybridDispatchLane::EventDriven,
                keys[cell],
                config.payloadBytes);
            const auto fixedPayload = benchmarkPayload(
                gamenet::examples::HybridDispatchLane::FixedTick,
                keys[cell],
                config.payloadBytes);
            auto encodedEvent = encoder.encode(eventPayload);
            auto encodedFixed = encoder.encode(fixedPayload);
            if (!encodedEvent || !encodedFixed) {
                throw std::runtime_error("benchmark payload encode failed");
            }
            encodedByCell[cell] = {
                .event = std::move(*encodedEvent),
                .fixed = std::move(*encodedFixed),
            };
        }

        std::atomic<bool> startClients{false};
        std::atomic<std::size_t> clientsDone{0};
        std::atomic<bool> clientFailure{false};
        std::vector<std::thread> clients;
        clients.reserve(config.clients);
        for (std::size_t clientIndex = 0;
             clientIndex < config.clients;
             ++clientIndex) {
            clients.emplace_back([&, clientIndex] {
                const auto fd = connectClient(profile->listenAddress());
                bool ok = waitUntil(
                    [&] { return startClients.load(std::memory_order_acquire); },
                    10s);
                gamenet::protocol::PacketFramer decoder(options.framing);
                const auto& encoded = encodedByCell[clientIndex % 2];
                std::size_t sent = 0;
                while (ok && sent != config.messagesPerClient) {
                    const auto batch = std::min(
                        config.batchMessages,
                        config.messagesPerClient - sent);
                    std::string outbound;
                    outbound.reserve(batch * (config.payloadBytes + 4U));
                    for (std::size_t offset = 0; offset < batch; ++offset) {
                        outbound += ((sent + offset) % 2 == 0)
                            ? encoded.event : encoded.fixed;
                    }
                    const auto deadline =
                        std::chrono::steady_clock::now() + 30s;
                    ok = writeAll(fd, outbound, deadline) &&
                        readBatch(fd, decoder, batch, deadline);
                    sent += batch;
                }
                gamenet::net::sockets::close(fd);
                if (!ok) clientFailure.store(true, std::memory_order_release);
                clientsDone.fetch_add(1, std::memory_order_release);
            });
        }

        auto startedAt = std::chrono::steady_clock::time_point{};
        auto finishedAt = std::chrono::steady_clock::time_point{};
        auto shutdownStartedAt = std::chrono::steady_clock::time_point{};
        gamenet::examples::MultiIoShardedHybridStopHandle stop;
        static_cast<void>(baseLoop.runEvery(1ms, [&] {
            const auto snapshot = profile->metrics();
            if (!startClients.load(std::memory_order_acquire) &&
                snapshot.connectionsOpened == config.clients &&
                snapshot.networkOwnerCount == 2) {
                startedAt = std::chrono::steady_clock::now();
                startClients.store(true, std::memory_order_release);
            }
            if (clientsDone.load(std::memory_order_acquire) != config.clients) {
                return;
            }
            if (!stop.valid()) {
                finishedAt = std::chrono::steady_clock::now();
                shutdownStartedAt = finishedAt;
                stop = profile->stopGracefully({.drainTimeout = 0ms});
                return;
            }
            if (stop.networkStop.wait_for(0s) == std::future_status::ready &&
                stop.logicStop.wait_for(0s) == std::future_status::ready) {
                shutdownMicros = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - shutdownStartedAt)
                        .count());
                baseLoop.quit();
            }
        }));
        static_cast<void>(baseLoop.runAfter(90s, [&] {
            clientFailure.store(true, std::memory_order_release);
            if (!startClients.exchange(true, std::memory_order_acq_rel)) {
                startedAt = std::chrono::steady_clock::now();
            }
            if (!stop.valid()) {
                stop = profile->stopGracefully({.drainTimeout = 0ms});
            }
            baseLoop.quit();
        }));
        baseLoop.loop();
        for (auto& client : clients) client.join();
        clientsSucceeded = !clientFailure.load(std::memory_order_acquire) &&
            startedAt != std::chrono::steady_clock::time_point{} &&
            finishedAt >= startedAt && stop.valid() &&
            stop.networkStop.wait_for(0s) == std::future_status::ready &&
            stop.logicStop.wait_for(0s) == std::future_status::ready;
        elapsedSeconds =
            std::chrono::duration<double>(finishedAt - startedAt).count();
        metrics = profile->metrics();
        profile.reset();
    }
    secondLogicThread.stop();
    firstLogicThread.stop();

    const auto workingSetAfter = sampleWorkingSetBytes();
    const auto workingSetDelta = workingSetAfter >= workingSetBefore
        ? static_cast<std::int64_t>(workingSetAfter - workingSetBefore)
        : -static_cast<std::int64_t>(workingSetBefore - workingSetAfter);
    const auto messages = config.clients * config.messagesPerClient;
    const auto eventMessages =
        config.clients * ((config.messagesPerClient + 1U) / 2U);
    const auto fixedMessages = messages - eventMessages;
    const auto bytes = messages * config.payloadBytes;
    const double messagesPerSecond = elapsedSeconds > 0.0
        ? static_cast<double>(messages) / elapsedSeconds
        : 0.0;
    const double eventMessagesPerSecond = elapsedSeconds > 0.0
        ? static_cast<double>(eventMessages) / elapsedSeconds
        : 0.0;
    const double fixedMessagesPerSecond = elapsedSeconds > 0.0
        ? static_cast<double>(fixedMessages) / elapsedSeconds
        : 0.0;
    const double mebibytesPerSecond = elapsedSeconds > 0.0
        ? static_cast<double>(bytes) / (1024.0 * 1024.0 * elapsedSeconds)
        : 0.0;

    std::uint64_t minimumCellHandlers =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximumCellHandlers = 0;
    std::size_t queueDepthHighWatermark = 0;
    for (const auto& cell : metrics.cells) {
        minimumCellHandlers = std::min(minimumCellHandlers, cell.handlerCalls);
        maximumCellHandlers = std::max(maximumCellHandlers, cell.handlerCalls);
        queueDepthHighWatermark = std::max(
            queueDepthHighWatermark, cell.depthHighWatermark);
    }
    const double cellImbalanceRatio = minimumCellHandlers > 0
        ? static_cast<double>(maximumCellHandlers) /
            static_cast<double>(minimumCellHandlers)
        : 0.0;
    const bool invariantPassed = clientsSucceeded &&
        metrics.commandsAccepted == messages &&
        metrics.eventCommandsAccepted == eventMessages &&
        metrics.fixedCommandsAccepted == fixedMessages &&
        metrics.handlerCalls == messages &&
        metrics.outputsAccepted == messages &&
        metrics.networkOwnerCount == 2 &&
        metrics.logicCellCount == 2 &&
        metrics.networkOwnerViolations == 0 &&
        metrics.logicOwnerViolations == 0 &&
        metrics.endpointOwnerViolations == 0 &&
        metrics.connectionOwnerMigrations == 0 &&
        metrics.orderViolations == 0 &&
        metrics.queueFullRejections == 0 &&
        metrics.profileTerminalFailures == 0 &&
        metrics.timerSetupFailures == 0 &&
        metrics.timerCancelFailures == 0 &&
        metrics.crossDomainHandoffs == 2U * messages &&
        minimumCellHandlers > 0;

    std::cout << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"schema\": \"gamenet.multi_io_sharded_hybrid_benchmark.v1\",\n"
              << "  \"build_type\": \"" << GAMENET_BENCHMARK_BUILD_TYPE
              << "\",\n"
              << "  \"status\": \""
              << (invariantPassed ? "ok" : "failed") << "\",\n"
              << "  \"clients\": " << config.clients << ",\n"
              << "  \"io_threads\": 2,\n"
              << "  \"logic_cells\": 2,\n"
              << "  \"messages_per_client\": "
              << config.messagesPerClient << ",\n"
              << "  \"payload_bytes\": " << config.payloadBytes << ",\n"
              << "  \"messages_per_second\": " << messagesPerSecond << ",\n"
              << "  \"event_messages_per_second\": "
              << eventMessagesPerSecond << ",\n"
              << "  \"fixed_tick_messages_per_second\": "
              << fixedMessagesPerSecond << ",\n"
              << "  \"mebibytes_per_second\": "
              << mebibytesPerSecond << ",\n"
              << "  \"cell_imbalance_ratio\": "
              << cellImbalanceRatio << ",\n"
              << "  \"queue_age_p999_us\": "
              << metrics.queueAgeP999Us << ",\n"
              << "  \"queue_depth_high_watermark\": "
              << queueDepthHighWatermark << ",\n"
              << "  \"fixed_head_deferrals\": "
              << metrics.fixedHeadDeferrals << ",\n"
              << "  \"working_set_before_bytes\": "
              << workingSetBefore << ",\n"
              << "  \"working_set_after_bytes\": "
              << workingSetAfter << ",\n"
              << "  \"working_set_delta_bytes\": "
              << workingSetDelta << ",\n"
              << "  \"cross_domain_handoffs\": "
              << metrics.crossDomainHandoffs << ",\n"
              << "  \"profile_shutdown_drain_us\": "
              << metrics.shutdownDrainWaitUs << ",\n"
              << "  \"shutdown_us\": " << shutdownMicros << "\n"
              << "}\n";
    return invariantPassed ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseConfig(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "gamenet_multi_io_sharded_hybrid_benchmark: "
                  << error.what() << '\n';
        return 2;
    }
}
